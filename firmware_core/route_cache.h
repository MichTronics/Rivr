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

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */

#define RCACHE_SIZE          64u         /**< Table capacity (fixed)           */
#define RCACHE_EXPIRY_MS     120000u     /**< Route expiry: 2 minutes          */
#define RCACHE_METRIC_INF    0xFFu       /**< "unreachable" sentinel           */

/* ── Entry flags ─────────────────────────────────────────────────────────── */

#define RCACHE_FLAG_VALID    0x01u  /**< Entry holds live data                 */
#define RCACHE_FLAG_PENDING  0x02u  /**< ROUTE_REQ sent; awaiting ROUTE_RPL   */
#define RCACHE_FLAG_DIRECT   0x04u  /**< dst_id is a direct (1-hop) neighbour  */

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
 *
 * @param cache      Route cache.
 * @param src_id     Original source node (from packet header).
 * @param from_id    Immediate neighbour that delivered the frame (0 if unknown).
 * @param hop_count  Hops already taken in the received packet (pkt->hop).
 * @param rssi_dbm   Received signal strength (negative; higher = better).
 * @param now_ms     Current monotonic millisecond timestamp.
 */
void route_cache_learn_rx(route_cache_t *cache,
                           uint32_t       src_id,
                           uint32_t       from_id,
                           uint8_t        hop_count,
                           int16_t        rssi_dbm,
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

#ifdef __cplusplus
}
#endif

#endif /* RIVR_ROUTE_CACHE_H */
