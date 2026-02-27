/**
 * @file  pending_queue.h
 * @brief Fixed-size pending-message queue for Phase-D route discovery.
 *
 * WHEN TO USE
 * ──────────────────────────────────────────────────────────────────────────
 * When rivr_sinks.c tries to TX a frame to a specific dst_id but the route
 * cache has no entry (cache miss), it:
 *   1. Calls pending_queue_enqueue() to save the wire-encoded frame here.
 *   2. Sends a PKT_ROUTE_REQ broadcast for that dst_id.
 *
 * When a PKT_ROUTE_RPL arrives in rivr_sources.c:
 *   1. route_cache is updated.
 *   2. pending_queue_drain_for_dst() re-encodes all saved frames as unicast
 *      via the newly resolved next_hop and pushes them to rf_tx_queue.
 *
 * MEMORY
 * ──────
 * PENDING_QUEUE_SIZE × (RF_MAX_PAYLOAD_LEN + sizeof(pending_entry_t)) bytes
 * sit in BSS.  At 16 × ~265 B ≈ 4.2 kB — fine for ESP32.
 *
 * EXPIRY
 * ──────
 * Entries older than PENDING_EXPIRY_MS are silently evicted to prevent
 * unbounded backlog if a ROUTE_RPL never arrives.
 */

#ifndef RIVR_PENDING_QUEUE_H
#define RIVR_PENDING_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "radio_sx1262.h"  /* RF_MAX_PAYLOAD_LEN, rf_tx_request_t, rb_t       */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */

#define PENDING_QUEUE_SIZE   16u     /**< Maximum simultaneously queued msgs  */
#define PENDING_EXPIRY_MS    30000u  /**< Evict unsent messages after 30 s    */

/* ── Types ───────────────────────────────────────────────────────────────── */

/** One pending outgoing message waiting for route resolution. */
typedef struct {
    uint32_t dst_id;                    /**< Original intended destination     */
    uint8_t  data[RF_MAX_PAYLOAD_LEN];  /**< Complete wire-encoded frame       */
    uint8_t  len;                       /**< Byte count in data[]              */
    uint32_t toa_us;                    /**< Time-on-Air estimate (µs)         */
    uint32_t enqueued_ms;               /**< tb_millis() at enqueue time       */
    bool     valid;                     /**< Slot occupied                     */
} pending_entry_t;

/** Complete pending-message queue state (BSS-safe). */
typedef struct {
    pending_entry_t entries[PENDING_QUEUE_SIZE];
    uint8_t         count;  /**< Number of currently valid entries             */
} pending_queue_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Zero-initialise a pending queue (call once at boot).
 */
void pending_queue_init(pending_queue_t *pq);

/**
 * @brief Save an outgoing frame pending route resolution.
 *
 * Expires stale entries first to reclaim space.
 *
 * @param pq       Queue state.
 * @param dst_id   Original destination node ID (must be non-zero).
 * @param data     Wire-encoded frame (as produced by protocol_encode).
 * @param len      Byte count of @p data.
 * @param toa_us   Time-on-Air estimate for re-transmission.
 * @param now_ms   Current monotonic timestamp.
 * @return true  → frame saved.
 * @return false → queue full after expiry.
 */
bool pending_queue_enqueue(pending_queue_t *pq,
                           uint32_t         dst_id,
                           const uint8_t   *data,
                           uint8_t          len,
                           uint32_t         toa_us,
                           uint32_t         now_ms);

/**
 * @brief Drain all pending frames destined for @p dst_id via @p next_hop.
 *
 * For each valid entry matching @p dst_id:
 *  • Decodes the wire frame.
 *  • Rewrites dst_id = @p next_hop, ttl = 1, due_ms = 0 (send immediately).
 *  • Re-encodes and pushes to @p tx_queue.
 *  • Clears the entry (regardless of TX-queue availability).
 *
 * @param pq        Queue state.
 * @param dst_id    Destination whose route was just resolved.
 * @param next_hop  Next hop toward @p dst_id (from route cache).
 * @param tx_queue  Hardware TX ring buffer (rf_tx_queue).
 * @param now_ms    Current monotonic timestamp.
 * @return Number of frames successfully pushed to @p tx_queue.
 */
uint8_t pending_queue_drain_for_dst(pending_queue_t *pq,
                                     uint32_t         dst_id,
                                     uint32_t         next_hop,
                                     rb_t            *tx_queue,
                                     uint32_t         now_ms);

/**
 * @brief Evict all entries older than PENDING_EXPIRY_MS.
 * @return Number of entries evicted.
 */
uint8_t pending_queue_expire(pending_queue_t *pq, uint32_t now_ms);

/**
 * @brief Count currently valid (non-expired) entries.
 */
uint8_t pending_queue_count(const pending_queue_t *pq);

/**
 * @brief Return pending-queue fill level as 0..100.
 *
 * Used for backpressure signalling: callers should throttle
 * non-control relay traffic when this value exceeds ~75.
 * Control packets (BEACON, ROUTE_REQ, ROUTE_RPL, ACK, PROG_PUSH)
 * must always bypass any backpressure gate.
 */
uint8_t pending_queue_pressure(const pending_queue_t *pq);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_PENDING_QUEUE_H */
