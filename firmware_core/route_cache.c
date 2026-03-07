/**
 * @file  route_cache.c
 * @brief Phase-D hybrid unicast route cache implementation.
 */

#include "route_cache.h"
#include "rivr_metrics.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* ── link score → metric conversion ────────────────────────────────────── *
 * Composite link score matching routing_neighbor_link_score():
 *   rssi_part = clamp(rssi_dbm + 140, 0, 80)   — 0..80
 *   snr_part  = clamp(snr_db   +  10, 0, 20)   — 0..20
 *   score     = rssi_part + snr_part             — 0..100
 * Higher score = better link.
 * ─────────────────────────────────────────────────────────────────────────── */
static uint8_t link_to_metric(int16_t rssi_dbm, int8_t snr_db)
{
    int32_t r = (int32_t)rssi_dbm + 140;
    if (r < 0)  r = 0;
    if (r > 80) r = 80;
    int32_t s = (int32_t)snr_db + 10;
    if (s < 0)  s = 0;
    if (s > 20) s = 20;
    return (uint8_t)(r + s);  /* 0..100: higher = better */
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
            /* Prefer: fewer hops; within same hop-count, better metric wins
             * only if improvement exceeds RCACHE_METRIC_HYSTERESIS, which
             * prevents route oscillation when two paths have similar scores. */
            if (hop_count < e->hop_count ||
                (hop_count == e->hop_count &&
                 (uint32_t)metric > (uint32_t)e->metric + RCACHE_METRIC_HYSTERESIS)) {
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

    g_rivr_metrics.rcache_evict++;

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
                           int8_t         snr_db,
                           uint32_t       now_ms)
{
    if (!cache || src_id == 0) return;

    /* Determine actual next_hop toward src_id:                              */
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

    uint8_t metric = link_to_metric(rssi_dbm, snr_db);
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

void route_cache_print(const route_cache_t *cache, uint32_t now_ms)
{
    if (!cache) return;
    printf("%-12s %-12s %4s %5s %6s  %s\r\n",
           "Destination", "NextHop", "Hops", "Score", "Age(s)", "Flags");
    uint8_t shown = 0u;
    for (uint8_t i = 0u; i < RCACHE_SIZE; i++) {
        const route_cache_entry_t *e = &cache->entries[i];
        if (!(e->flags & RCACHE_FLAG_VALID)) continue;
        uint32_t age_ms = now_ms - e->last_seen_ms;
        if (age_ms > RCACHE_EXPIRY_MS) continue;
        char flags[4] = "---";
        if (e->flags & RCACHE_FLAG_VALID)   flags[0] = 'U';
        if (e->flags & RCACHE_FLAG_DIRECT)  flags[1] = 'D';
        if (e->flags & RCACHE_FLAG_PENDING) flags[2] = 'P';
        printf("0x%08lX   0x%08lX %4u %5u %6lu  %s\r\n",
               (unsigned long)e->dst_id,
               (unsigned long)e->next_hop,
               (unsigned)e->hop_count,
               (unsigned)e->metric,
               (unsigned long)(age_ms / 1000u),
               flags);
        shown++;
    }
    if (shown == 0u) printf("  (no live routes)\r\n");
}
bool route_cache_can_reply_for_dst(route_cache_t *cache,
                                    uint32_t       dst_id,
                                    uint32_t       now_ms)
{
    /* Gate 1: valid, non-expired entry must exist (lazy expiry applied). */
    const route_cache_entry_t *e = route_cache_lookup(cache, dst_id, now_ms);
    if (!e) return false;

    /* Gate 2: entry must not be pending — route is tentative until we
     * receive a ROUTE_RPL for our own outstanding request. */
    if (e->flags & RCACHE_FLAG_PENDING) return false;

    /* Gate 3: metric must meet the minimum quality threshold.  Paths with a
     * combined RSSI+SNR score below RCACHE_REPLY_MIN_METRIC are marginal;
     * advertising them generates noisy control-plane traffic with little
     * benefit to the requester. */
    if (e->metric < RCACHE_REPLY_MIN_METRIC) return false;

    /* Gate 4: route must not pass through too many hops.  Deep cached paths
     * age faster and are less reliable than a direct reply from the target,
     * so we stay silent and let the target speak for itself. */
    if (e->hop_count > RCACHE_REPLY_MAX_HOPS) return false;

    return true;
}