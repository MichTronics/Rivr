/**
 * @file  route_cache.c
 * @brief Phase-D hybrid unicast route cache implementation.
 */

#include "route_cache.h"
#include <string.h>

/* ── rssi → metric conversion ────────────────────────────────────────────── *
 * metric = 0    → excellent (rssi ≈ -50 dBm)
 * metric = 255  → unreachable
 * The mapping is: metric = clamp(255 + rssi_dbm, 0, 254)
 *   rssi = -50 → metric = 205  (good direct link)
 *   rssi = -100 → metric = 155 (decent relay)
 *   rssi = -130 → metric = 125 (weak)
 * Lower hop_count breaks ties.
 * ─────────────────────────────────────────────────────────────────────────── */
static uint8_t rssi_to_metric(int16_t rssi_dbm)
{
    int32_t m = 255 + (int32_t)rssi_dbm;
    if (m < 0)   m = 0;
    if (m > 254) m = 254;
    return (uint8_t)m;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static bool entry_is_alive(const route_cache_entry_t *e, uint32_t now_ms)
{
    if (!(e->flags & RCACHE_FLAG_VALID)) return false;
    uint32_t age = now_ms - e->last_seen_ms;
    return age <= RCACHE_EXPIRY_MS;
}

/* Expire a single entry in-place, return true if it was active. */
static bool expire_entry(route_cache_entry_t *e, uint32_t now_ms)
{
    if (!(e->flags & RCACHE_FLAG_VALID)) return false;
    uint32_t age = now_ms - e->last_seen_ms;
    if (age > RCACHE_EXPIRY_MS) {
        memset(e, 0, sizeof(*e));
        return true;
    }
    return false;
}

/* ── API implementation ──────────────────────────────────────────────────── */

void route_cache_init(route_cache_t *cache)
{
    if (!cache) return;
    memset(cache, 0, sizeof(*cache));
}

const route_cache_entry_t *route_cache_lookup(route_cache_t *cache,
                                               uint32_t       dst_id,
                                               uint32_t       now_ms)
{
    if (!cache || dst_id == 0) return NULL;

    for (uint8_t i = 0; i < RCACHE_SIZE; i++) {
        route_cache_entry_t *e = &cache->entries[i];
        if (e->dst_id == dst_id) {
            if (expire_entry(e, now_ms)) return NULL;   /* lazily evicted */
            if (e->flags & RCACHE_FLAG_VALID)   return e;
        }
    }
    return NULL;
}

void route_cache_update(route_cache_t *cache,
                        uint32_t       dst_id,
                        uint32_t       next_hop,
                        uint8_t        hop_count,
                        uint8_t        metric,
                        uint8_t        flags,
                        uint32_t       now_ms)
{
    if (!cache || dst_id == 0 || next_hop == 0) return;

    /* Search for an existing entry for this dst_id */
    for (uint8_t i = 0; i < RCACHE_SIZE; i++) {
        route_cache_entry_t *e = &cache->entries[i];

        /* Reclaim expired entry for any dst_id */
        expire_entry(e, now_ms);

        if (e->dst_id == dst_id && (e->flags & RCACHE_FLAG_VALID)) {
            /* Prefer: fewer hops, then better (lower) metric */
            if (hop_count < e->hop_count ||
                (hop_count == e->hop_count && metric > e->metric)) {
                e->next_hop      = next_hop;
                e->hop_count     = hop_count;
                e->metric        = metric;
                e->flags         = (uint8_t)(flags | RCACHE_FLAG_VALID);
            }
            /* Always refresh timestamp so route doesn't expire prematurely */
            e->last_seen_ms = now_ms;
            return;
        }
    }

    /* Not found — find an empty or expired slot */
    for (uint8_t i = 0; i < RCACHE_SIZE; i++) {
        route_cache_entry_t *e = &cache->entries[i];
        if (!(e->flags & RCACHE_FLAG_VALID)) {
            e->dst_id       = dst_id;
            e->next_hop     = next_hop;
            e->last_seen_ms = now_ms;
            e->hop_count    = hop_count;
            e->metric       = metric;
            e->flags        = (uint8_t)(flags | RCACHE_FLAG_VALID);
            if (cache->count < RCACHE_SIZE) cache->count++;
            return;
        }
    }

    /* Table full — evict round-robin */
    uint8_t idx = cache->evict_hand;
    cache->evict_hand = (uint8_t)((idx + 1u) % RCACHE_SIZE);

    route_cache_entry_t *e = &cache->entries[idx];
    e->dst_id       = dst_id;
    e->next_hop     = next_hop;
    e->last_seen_ms = now_ms;
    e->hop_count    = hop_count;
    e->metric       = metric;
    e->flags        = (uint8_t)(flags | RCACHE_FLAG_VALID);
}

void route_cache_learn_rx(route_cache_t *cache,
                           uint32_t       src_id,
                           uint32_t       from_id,
                           uint8_t        hop_count,
                           int16_t        rssi_dbm,
                           uint32_t       now_ms)
{
    if (!cache || src_id == 0) return;

    /* Determine actual next_hop toward src_id:
     *  - hop_count == 0 → src is a direct neighbour; next_hop = src_id
     *  - hop_count >  0 → src is behind a relay; next_hop = from_id (the relay)
     *                     Only learn if we know who the relay is.
     */
    uint32_t next_hop;
    uint8_t  nhops;
    if (hop_count == 0) {
        next_hop = src_id;           /* direct */
        nhops    = 1u;
    } else if (from_id != 0 && from_id != src_id) {
        next_hop = from_id;          /* relay forwarded it to us */
        nhops    = (uint8_t)(hop_count + 1u);
    } else {
        return;   /* relay unknown, can't learn reliable route */
    }

    uint8_t metric = rssi_to_metric(rssi_dbm);
    uint8_t f = RCACHE_FLAG_VALID | (hop_count == 0 ? RCACHE_FLAG_DIRECT : 0u);
    route_cache_update(cache, src_id, next_hop, nhops, metric, f, now_ms);
}

rcache_tx_decision_t route_cache_tx_decide(route_cache_t *cache,
                                            uint32_t       dst_id,
                                            uint32_t       now_ms,
                                            uint32_t      *next_hop_out)
{
    if (next_hop_out) *next_hop_out = 0;
    if (!cache || dst_id == 0) return RCACHE_TX_FLOOD;   /* broadcast always floods */

    const route_cache_entry_t *e = route_cache_lookup(cache, dst_id, now_ms);
    if (!e) return RCACHE_TX_FLOOD;

    if (next_hop_out) *next_hop_out = e->next_hop;
    return RCACHE_TX_UNICAST;
}

uint8_t route_cache_expire(route_cache_t *cache, uint32_t now_ms)
{
    if (!cache) return 0;
    uint8_t evicted = 0;
    for (uint8_t i = 0; i < RCACHE_SIZE; i++) {
        if (expire_entry(&cache->entries[i], now_ms)) {
            if (cache->count > 0) cache->count--;
            evicted++;
        }
    }
    return evicted;
}

uint8_t route_cache_count(const route_cache_t *cache, uint32_t now_ms)
{
    if (!cache) return 0;
    uint8_t alive = 0;
    for (uint8_t i = 0; i < RCACHE_SIZE; i++) {
        if (entry_is_alive(&cache->entries[i], now_ms)) alive++;
    }
    return alive;
}
