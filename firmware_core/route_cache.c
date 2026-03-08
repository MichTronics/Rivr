/**
 * @file  route_cache.c
 * @brief Phase-D hybrid unicast route cache implementation.
 */

#include "route_cache.h"
#include "neighbor_table.h"
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

/* ── Composite route score ───────────────────────────────────────────────── *
 * Factors in:                                                                *
 *   1. entry->metric        (RSSI+SNR composite, 0..100)                    *
 *   2. hop-count penalty    (1 hop=100%, 2 hops=75%, 3+ hops=50%)          *
 *   3. age decay            (linear toward 0 at RCACHE_EXPIRY_MS)           *
 *   4. loss-rate penalty    (from standalone ntbl, if peer is known)        *
 * Result is in [0, 100].                                                     *
 * ─────────────────────────────────────────────────────────────────────────── */
static uint32_t entry_composite_score(const route_cache_entry_t   *e,
                                      const rivr_neighbor_table_t *ntbl,
                                      uint32_t                     now_ms)
{
    uint32_t age = now_ms - e->last_seen_ms;
    if (age >= RCACHE_EXPIRY_MS) return 0u;

    /* 1. Base metric (0..100) */
    uint32_t score = e->metric;

    /* 2. Hop-count weight */
    if (e->hop_count >= 3u) {
        score = score * 50u / 100u;
    } else if (e->hop_count == 2u) {
        score = score * 75u / 100u;
    }
    /* hop_count == 1 → no penalty */

    /* 3. Linear age decay */
    score = score * (RCACHE_EXPIRY_MS - age) / RCACHE_EXPIRY_MS;

    /* 4. Quality penalty from standalone neighbor table.
     * Phase 2 (RIVR_FEATURE_AIRTIME_ROUTING=1): replace linear loss-rate
     * penalty with ETX×8 weighting — links with higher delivery ratio score
     * proportionally better.
     * Phase 0–1 default: classic loss-rate scaling (no behavior change).   */
    if (ntbl) {
        const rivr_neighbor_t *n = neighbor_find(ntbl, e->next_hop, now_ms);
        if (n) {
#if RIVR_FEATURE_AIRTIME_ROUTING
            uint32_t etx = (uint32_t)(n->etx_x8 >= 8u ? n->etx_x8 : 8u);
            score = score * 8u / etx;
#else
            score = score * (uint32_t)(100u - n->loss_rate) / 100u;
#endif
        }
    }

    return score > 100u ? 100u : score;
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
            if (expire_entry(e, now_ms)) {
                g_rivr_metrics.route_cache_miss_total++;
                return NULL;   /* lazily evicted */
            }
            if (e->flags & RCACHE_FLAG_VALID) {
                g_rivr_metrics.route_cache_hit_total++;
                return e;
            }
        }
    }
    g_rivr_metrics.route_cache_miss_total++;
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

bool route_cache_best_hop(route_cache_t              *cache,
                          const rivr_neighbor_table_t *ntbl,
                          uint32_t                    dst_id,
                          uint32_t                    now_ms,
                          rivr_route_t               *out)
{
    if (!cache || !out || dst_id == 0u) return false;

    /* ── Tier 1: route-cache hit ────────────────────────────────────────── *
     * Scan the whole table (not just the first match) to pick the highest-  
     * scoring path when more than one cached route covers the same dst_id.  
     * This can happen after a path change teaches a new route while the old  
     * route is still fresh.                                                  */
    const route_cache_entry_t *best_entry = NULL;
    uint32_t                   best_score = 0u;

    for (uint8_t i = 0u; i < RCACHE_SIZE; i++) {
        route_cache_entry_t *e = &cache->entries[i];
        if (e->dst_id != dst_id) continue;
        if (expire_entry(e, now_ms)) continue;          /* lazy expiry */
        if (!(e->flags & RCACHE_FLAG_VALID))   continue;
        if (  e->flags & RCACHE_FLAG_PENDING)  continue; /* tentative */

        uint32_t sc = entry_composite_score(e, ntbl, now_ms);
        if (sc > best_score) {
            best_score = sc;
            best_entry = e;
        }
    }

    if (best_entry) {
        out->dest_id    = dst_id;
        out->next_hop_id = best_entry->next_hop;
        out->metric     = (uint16_t)best_score;
        out->hop_count  = best_entry->hop_count;
        out->expires_ms = best_entry->last_seen_ms + RCACHE_EXPIRY_MS;
        g_rivr_metrics.route_cache_hit_total++;
        g_rivr_metrics.neighbor_route_used_total++;
#if RIVR_FEATURE_AIRTIME_ROUTING
        g_rivr_metrics.airtime_route_selected_total++;
#endif
        return true;
    }

    /* ── Tier 2: neighbor-best forwarder ───────────────────────────────── *
     * No cache entry for dst_id but we do have a live direct neighbor that   
     * can forward traffic.  Promote that neighbor as a best-effort next hop. 
     * This avoids a full flood when the mesh is dense and the next hop is     
     * very likely to be able to reach the destination.                        */
    if (ntbl) {
        const rivr_neighbor_t *nb = neighbor_best(ntbl, now_ms);
        if (nb && (nb->flags & NTABLE_FLAG_DIRECT)) {
            uint8_t nb_score = neighbor_link_score(nb, now_ms);
            if (nb_score >= NTABLE_SCORE_UNICAST_MIN) {
                out->dest_id    = dst_id;
                out->next_hop_id = nb->neighbor_id;
                out->metric     = nb_score;
                out->hop_count  = 2u;  /* estimate: 1 to neighbor + 1 to dst */
                out->expires_ms = nb->last_seen_ms + NTABLE_EXPIRY_MS;
                g_rivr_metrics.neighbor_route_used_total++;
#if RIVR_FEATURE_AIRTIME_ROUTING
                g_rivr_metrics.airtime_route_selected_total++;
#endif
                return true;
            }
        }
    }

    /* ── Tier 3: no usable next hop ───────────────────────────────────────── */
    g_rivr_metrics.route_cache_miss_total++;
    g_rivr_metrics.neighbor_route_failed_total++;
#if RIVR_FEATURE_AIRTIME_ROUTING
    g_rivr_metrics.airtime_route_fallback_total++;
#endif
    return false;
}