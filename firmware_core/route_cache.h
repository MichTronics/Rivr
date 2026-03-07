/**
 * @file  route_cache.h
 * @brief Phase-D hybrid unicast route cache — fixed-size BSS allocation.
 *
 * DESIGN
 * ──────
 *  64-entry flat table keyed on dst_id.  On a cache hit the application layer
 *  rewrites dst_id → next_hop and sets TTL = 1 so the packet is sent directly
 *  without further flooding.
 *
 *  On a cache miss the caller may either:
 *    a) Flood the packet (fallback), or
 *    b) Issue a ROUTE_REQ and queue the message as PENDING.
 *
 *  Reverse learning: every received packet teaches us a route to src_id via
 *  the immediate-neighbour that delivered it (`from_id`).  This is similar to
 *  AODV reverse-path learning and requires no extra control traffic for nodes
 *  that already observed a bidirectional link.
 *
 * EXPIRY
 * ──────
 *  Entries expire after RCACHE_EXPIRY_MS (default 2 min).  They are lazily
 *  purged on the next lookup / update for that dst_id.
 *
 * MEMORY
 * ──────
 *  sizeof(route_cache_t) ≈ 64 × 24 = 1536 B → well within ESP32 DRAM.
 *  All state in BSS; no heap.
 */

#ifndef RIVR_ROUTE_CACHE_H
#define RIVR_ROUTE_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/feature_flags.h"    /* RIVR_ROUTE_CACHE_SIZE role-specific default */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */

/** Route cache table capacity.
 *  Sized by RIVR_ROUTE_CACHE_SIZE from hal/feature_flags.h:
 *    CLIENT=32, REPEATER/GATEWAY/generic=64.
 *  Override per-board via -DRIVR_ROUTE_CACHE_SIZE=N. */
#define RCACHE_SIZE          RIVR_ROUTE_CACHE_SIZE
#define RCACHE_EXPIRY_MS     120000u     /**< Route expiry: 2 minutes          */
#define RCACHE_METRIC_INF    0xFFu       /**< "unreachable" sentinel           */
#define RCACHE_METRIC_HYSTERESIS 10u    /**< Min score gain needed to swap routes */

/* ── Reply-eligibility thresholds ───────────────────────────────────────── *
 * ROUTE_RPL reply-eligibility policy for cached-route responders.           *
 *                                                                            *
 * A node may only volunteer a cached route in response to a ROUTE_REQ if:  *
 *   1. Entry is valid and non-expired  (checked by route_cache_lookup).      *
 *   2. RCACHE_FLAG_PENDING is NOT set  (we are still awaiting our own REQ;  *
 *      the cached data is tentative and we cannot vouch for it yet).         *
 *   3. metric >= RCACHE_REPLY_MIN_METRIC  (not a barely-heard marginal path; *
 *      prevents advertising paths the requester could not use reliably).     *
 *   4. hop_count <= RCACHE_REPLY_MAX_HOPS  (reasonably direct; deep cached  *
 *      paths are unlikely to beat a direct answer from the target itself,   *
 *      and age faster than short paths).                                     *
 *                                                                            *
 * These gates keep the control plane lean and deterministic: weak, deep, or  *
 * pending routes contribute no unsolicited ROUTE_RPL traffic.               *
 * ─────────────────────────────────────────────────────────────────────────── */
/** Minimum metric (0..100 composite score) for a cached reply to qualify. */
#define RCACHE_REPLY_MIN_METRIC  30u
/** Maximum hop depth of a qualifying cached route (inclusive). */
#define RCACHE_REPLY_MAX_HOPS     3u

/* ── Entry flags ─────────────────────────────────────────────────────────── */

#define RCACHE_FLAG_VALID    0x01u  /**< Entry holds live data                 */
#define RCACHE_FLAG_PENDING  0x02u  /**< ROUTE_REQ sent; awaiting ROUTE_RPL   */
#define RCACHE_FLAG_DIRECT   0x04u  /**< dst_id is a direct (1-hop) neighbour  */

/* ── Public route view (read-only snapshot) ─────────────────────────────── */

/**
 * Stable, caller-facing route description.
 *
 * Mirrors route_cache_entry_t but uses the naming requested by the routing
 * upgrade: dest_id / next_hop_id / metric / hop_count / expires_ms.
 * Callers receive this by value from route_cache_best_hop() so they never
 * hold a pointer into the mutable cache table.
 */
typedef struct {
    uint32_t dest_id;      /**< Destination node ID                             */
    uint32_t next_hop_id;  /**< Immediate next hop toward dest                  */
    uint16_t metric;       /**< Composite quality score 0–100 (higher = better) */
    uint8_t  hop_count;    /**< Total hops from us to dest (1 = direct)         */
    uint32_t expires_ms;   /**< Absolute expiry timestamp (last_seen + EXPIRY)  */
} rivr_route_t;

/* ── Route-cache entry ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t dst_id;          /**< Destination node ID (0 = empty slot)       */
    uint32_t next_hop;        /**< Immediate next hop toward dst               */
    uint32_t last_seen_ms;    /**< Monotonic ms of last confirmation           */
    uint8_t  hop_count;       /**< Total hops from us to dst (1 = direct)      */
    uint8_t  metric;          /**< Link quality — lower is better (0 = best)   */
    uint8_t  flags;           /**< RCACHE_FLAG_* bitmask                       */
    uint8_t  _pad;            /**< Alignment padding                           */
} route_cache_entry_t;

/* ── Route-cache table ───────────────────────────────────────────────────── */

typedef struct {
    route_cache_entry_t entries[RCACHE_SIZE];
    uint8_t             count;       /**< Number of valid (non-expired) entries */
    uint8_t             evict_hand;  /**< Round-robin eviction cursor           */
} route_cache_t;

/* ── TX decision ─────────────────────────────────────────────────────────── */

typedef enum {
    RCACHE_TX_FLOOD,       /**< No cache hit — broadcast flood              */
    RCACHE_TX_UNICAST,     /**< Cache hit — send to entry->next_hop only    */
    RCACHE_TX_PENDING,     /**< ROUTE_REQ just issued; caller should retry  */
} rcache_tx_decision_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise the route cache (zero all entries).
 */
void route_cache_init(route_cache_t *cache);

/**
 * @brief Look up the best route to @p dst_id.
 *
 * Lazily expires the entry if it is older than RCACHE_EXPIRY_MS.
 *
 * @param cache     Route cache.
 * @param dst_id    Destination node ID.
 * @param now_ms    Current monotonic millisecond timestamp.
 * @return Pointer to the matching valid entry, or NULL (miss / expired).
 */
const route_cache_entry_t *route_cache_lookup(route_cache_t *cache,
                                               uint32_t       dst_id,
                                               uint32_t       now_ms);

/**
 * @brief Insert or refresh a route to @p dst_id.
 *
 * If a better route (fewer hops / better metric) already exists the update
 * is skipped unless the existing entry has expired.
 *
 * @param cache      Route cache.
 * @param dst_id     Destination node ID.
 * @param next_hop   Next-hop node ID to reach @p dst_id.
 * @param hop_count  Total hops from us to @p dst_id.
 * @param metric     Link quality metric (lower = better; e.g. (255 + rssi)).
 * @param flags      RCACHE_FLAG_* to set on the entry.
 * @param now_ms     Current monotonic millisecond timestamp.
 */
void route_cache_update(route_cache_t *cache,
                        uint32_t       dst_id,
                        uint32_t       next_hop,
                        uint8_t        hop_count,
                        uint8_t        metric,
                        uint8_t        flags,
                        uint32_t       now_ms);

/**
 * @brief Learn a route to @p src_id via @p from_id (reverse-path learning).
 *
 * Called on every successfully decoded inbound frame.
 * - If @p hop_count == 0 the source is a direct neighbour.
 * - If @p from_id == 0 and hop_count == 0, derives from_id = src_id.
 * - Skips learning if from_id == 0 AND hop_count > 0 (relay unknown).
 * - Metric is computed as a composite link score from RSSI + SNR (0..100).
 *
 * @param cache      Route cache.
 * @param src_id     Original source node (from packet header).
 * @param from_id    Immediate neighbour that delivered the frame (0 if unknown).
 * @param hop_count  Hops already taken in the received packet (pkt->hop).
 * @param rssi_dbm   Received signal strength (negative; higher = better).
 * @param snr_db     Signal-to-noise ratio in dB.
 * @param now_ms     Current monotonic millisecond timestamp.
 */
void route_cache_learn_rx(route_cache_t *cache,
                           uint32_t       src_id,
                           uint32_t       from_id,
                           uint8_t        hop_count,
                           int16_t        rssi_dbm,
                           int8_t         snr_db,
                           uint32_t       now_ms);

/**
 * @brief Determine how to transmit a packet destined for @p dst_id.
 *
 * Returns RCACHE_TX_UNICAST if a live cache entry exists and sets
 * *@p next_hop_out to the next-hop node ID.
 *
 * Returns RCACHE_TX_FLOOD if no entry is known.
 *
 * dst_id == 0 (broadcast) always returns RCACHE_TX_FLOOD.
 *
 * @param cache          Route cache.
 * @param dst_id         Intended destination (0 = broadcast).
 * @param now_ms         Current monotonic millisecond timestamp.
 * @param[out] next_hop_out  Set to next-hop on UNICAST; 0 otherwise.
 * @return RCACHE_TX_UNICAST or RCACHE_TX_FLOOD.
 */
rcache_tx_decision_t route_cache_tx_decide(route_cache_t *cache,
                                            uint32_t       dst_id,
                                            uint32_t       now_ms,
                                            uint32_t      *next_hop_out);

/* Pull in neighbor-table types here (forward usage by route_cache_best_hop). */
#include "neighbor_table.h"

/**
 * @brief Select the best next hop for @p dst_id using full neighbor quality.
 *
 * Three-tier decision (in order):
 *   1. Route-cache hit — selects the entry with the highest composite score.
 *      Composite score = entry->metric (RSSI+SNR 0..100)
 *                      × hop_weight (1.0 for 1 hop, 0.75 for 2, 0.5 for 3+)
 *                      × time_decay (linear, same formula as neighbor_link_score)
 *                      — penalty for loss_rate from @p ntbl (if peer known).
 *   2. Neighbor-best forwarder — no cache entry for dst, but ntbl has a live
 *      direct neighbor with score >= NTABLE_SCORE_UNICAST_MIN; that neighbor
 *      is promoted as a best-effort next hop (will flood-forward to dst).
 *   3. Returns false — caller should flood.
 *
 * @param cache     Route cache.
 * @param ntbl      Standalone neighbor-quality table (may be NULL).
 * @param dst_id    Destination node ID.
 * @param now_ms    Current monotonic timestamp in ms.
 * @param[out] out  Populated with the chosen route on true return.
 * @return true if a unicast next hop was selected; false → flood.
 */
bool route_cache_best_hop(route_cache_t              *cache,
                          const rivr_neighbor_table_t *ntbl,
                          uint32_t                    dst_id,
                          uint32_t                    now_ms,
                          rivr_route_t               *out);

/**
 * @brief Expire stale cache entries.
 *
 * Sweeps the whole table and clears entries older than RCACHE_EXPIRY_MS.
 * Call this periodically (e.g. every 10 s) to reclaim slots.
 *
 * @param cache   Route cache.
 * @param now_ms  Current monotonic millisecond timestamp.
 * @return Number of entries that were expired.
 */
uint8_t route_cache_expire(route_cache_t *cache, uint32_t now_ms);

/**
 * @brief Return number of living (valid, non-expired) entries.
 */
uint8_t route_cache_count(const route_cache_t *cache, uint32_t now_ms);

/**
 * @brief Print the route cache as a human-readable table via printf().
 *
 * Output format (one row per live entry):
 *   Destination  NextHop     Hops  Score  Age(s)  Flags
 *   Flags: U=unicast-valid D=direct P=pending
 *
 * @param cache   Route cache.
 * @param now_ms  Current monotonic millisecond timestamp.
 */
void route_cache_print(const route_cache_t *cache, uint32_t now_ms);

/**
 * @brief Test whether a cached route qualifies for replying to a ROUTE_REQ.
 *
 * Implements the reply-eligibility policy:
 *
 *  1. A valid, non-expired entry for @p dst_id must exist.
 *  2. The entry must NOT carry RCACHE_FLAG_PENDING (route is tentative —
 *     we are still awaiting our own ROUTE_RPL for this destination).
 *  3. entry->metric >= RCACHE_REPLY_MIN_METRIC (not a barely-heard path).
 *  4. entry->hop_count <= RCACHE_REPLY_MAX_HOPS (not a deep stale chain).
 *
 * No memory allocation; safe to call from any context.
 *
 * @param cache   Route cache to query.
 * @param dst_id  Destination node ID to test.
 * @param now_ms  Monotonic millisecond timestamp (for expiry check).
 * @return true  → caller should send a ROUTE_RPL for @p dst_id.
 * @return false → stay silent (no entry / expired / pending / weak / deep).
 */
bool route_cache_can_reply_for_dst(route_cache_t *cache,
                                    uint32_t       dst_id,
                                    uint32_t       now_ms);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_ROUTE_CACHE_H */
