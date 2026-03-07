/**
 * @file  retry_table.h
 * @brief Fixed-size ACK-wait / retry table for directed unicast reliability.
 *
 * MECHANISM
 * ─────────
 * When a directed (dst_id != 0) data frame is sent with PKT_FLAG_ACK_REQ set,
 * an entry is inserted here keyed by (ack_src_id, cur_pkt_id).
 *
 *   • ACK received     → retry_table_ack() clears the entry (delivery confirmed).
 *   • Timeout fires    → retry_table_tick() re-transmits with a fresh pkt_id
 *                        (so intermediate dedupe caches don't suppress the retry)
 *                        and doubles the timeout window (exponential backoff).
 *   • Retries exhaust  → entry evicted; a fallback flood frame is enqueued.
 *
 * ACK PAYLOAD WIRE FORMAT  (ACK_PAYLOAD_LEN = 6 bytes, encoded in PKT_ACK):
 *   [0–3]  ack_src_id  u32 LE — src_id of the frame being acknowledged
 *   [4–5]  ack_pkt_id  u16 LE — pkt_id currently active in the sender's retry entry
 *
 * RETRY TIMING (LoRa friendly)
 * ─────────────────────────────
 *   RETRY_TIMEOUT_MS = 4 000 ms  — first ACK wait (covers 4-hop LoRa RTT)
 *   Each retry doubles the window: 4 s → 8 s → 16 s (total ≈ 28 s)
 *   RETRY_MAX        = 3         — three re-TX attempts before fallback flood
 *
 * MEMORY
 * ──────
 * RETRY_TABLE_SIZE × sizeof(retry_entry_t) bytes in BSS.
 * At 16 × (RF_MAX_PAYLOAD_LEN + ~24 B) ≈ 4.4 kB — fits easily on ESP32.
 * Zero-initialised at link time (BSS); no explicit init call required, but
 * retry_table_init() may be called to reset the table at runtime.
 */

#ifndef RIVR_RETRY_TABLE_H
#define RIVR_RETRY_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/feature_flags.h"    /* RIVR_RETRY_TABLE_SIZE role-specific default */
#include "radio_sx1262.h"   /* RF_MAX_PAYLOAD_LEN, rf_tx_request_t, rb_t     */
#include "protocol.h"       /* RIVR_PKT_HDR_LEN, RIVR_PKT_CRC_LEN, PKT_*    */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */

/** ACK-wait / retry table capacity.
 *  Sized by RIVR_RETRY_TABLE_SIZE from hal/feature_flags.h:
 *    CLIENT=8, REPEATER/GATEWAY=32, generic=16.
 *  Override per-board via -DRIVR_RETRY_TABLE_SIZE=N. */
#define RETRY_TABLE_SIZE    RIVR_RETRY_TABLE_SIZE
#define RETRY_MAX            3u    /**< Re-TX attempts after first TX (total 4) */
#define RETRY_TIMEOUT_MS  4000u    /**< Initial ACK wait interval (ms)          */

/* ── Types ───────────────────────────────────────────────────────────────── */

/**
 * One entry awaiting an ACK.
 *
 * The stored wire frame (data/len) is patched in-place on each retry:
 *  - bytes [19..20] (pkt_id) are overwritten with a fresh monotonic value.
 *  - CRC-16 is recomputed over the patched frame.
 * cur_pkt_id always matches the pkt_id currently in data[] so the ACK lookup
 * key stays in sync.
 */
typedef struct {
    uint32_t ack_src_id;              /**< src_id of the frame (= our node_id)    */
    uint16_t cur_pkt_id;              /**< pkt_id currently encoded in data[]      */
    uint32_t dst_id;                  /**< Final destination (for fallback label)  */
    uint32_t next_hop;                /**< Resolved next hop (dst in TX frame)     */
    uint8_t  data[RF_MAX_PAYLOAD_LEN]; /**< Wire-encoded frame (patched per retry) */
    uint8_t  len;                     /**< Byte count in data[]                    */
    uint32_t toa_us;                  /**< Time-on-Air estimate                    */
    uint32_t next_retry_ms;           /**< Monotonic ms of next retry/eviction     */
    uint32_t timeout_interval_ms;     /**< Current wait window (doubles per retry) */
    uint8_t  retries_left;            /**< Decremented on each re-TX               */
    bool     valid;                   /**< Slot occupied                           */
} retry_entry_t;

/** Complete retry table state (BSS-safe). */
typedef struct {
    retry_entry_t entries[RETRY_TABLE_SIZE];
    uint8_t       count;   /**< Number of valid entries                          */
} retry_table_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise (or reset) the retry table.
 *
 * Zeroes all entries and the count.  Because the struct lives in BSS this is
 * only required if you want to reset the table after initial boot.
 */
void retry_table_init(retry_table_t *rt);

/**
 * @brief Store a new entry awaiting ACK.
 *
 * @p data must already have PKT_FLAG_ACK_REQ set in byte [4].
 *
 * @param rt            Retry table.
 * @param ack_src_id    Our node_id (the expected ack_src_id from the receiver).
 * @param cur_pkt_id    pkt_id currently encoded in the wire frame.
 * @param dst_id        Final destination (for fallback diagnostics).
 * @param next_hop      Resolved next hop (already encoded in data[], kept here
 *                      for logging only).
 * @param data          Wire-encoded frame bytes.
 * @param len           Frame length.
 * @param toa_us        Estimated Time-on-Air.
 * @param now_ms        tb_millis() at time of first TX.
 * @return true on success; false if the table is full.
 */
bool retry_table_enqueue(retry_table_t *rt,
                         uint32_t ack_src_id, uint16_t cur_pkt_id,
                         uint32_t dst_id, uint32_t next_hop,
                         const uint8_t *data, uint8_t len,
                         uint32_t toa_us, uint32_t now_ms);

/**
 * @brief Clear the entry matching (ack_src_id, ack_pkt_id) on ACK receipt.
 *
 * The receiver sends a PKT_ACK with payload {ack_src_id, ack_pkt_id}.
 * We match on (ack_src_id == entry.ack_src_id && ack_pkt_id == entry.cur_pkt_id).
 * The cur_pkt_id is updated on each retry, so this always tracks the most
 * recently transmitted pkt_id.
 *
 * @return true if a matching entry was found and cleared.
 */
bool retry_table_ack(retry_table_t *rt,
                     uint32_t ack_src_id, uint16_t ack_pkt_id);

/**
 * @brief Tick: process expired entries (retry or fallback).
 *
 * For each entry where now_ms >= next_retry_ms:
 *
 *   retries_left > 0:
 *     1. Increment *pkt_id_counter; patch bytes [19..20] with new pkt_id.
 *     2. Recompute CRC-16 over the patched frame.
 *     3. Push rf_tx_request_t to tx_queue.
 *     4. Decrement retries_left; double timeout_interval_ms.
 *     5. Increment g_rivr_metrics.retry_fail_total if push fails.
 *
 *   retries_left == 0 (all retries exhausted):
 *     1. Build fallback flood: dst=0, ttl=RIVR_FALLBACK_TTL, PKT_FLAG_FALLBACK.
 *     2. Push to tx_queue; increment retry_fallback_total metric.
 *     3. Clear the entry.
 *
 * Call once per main-loop iteration (O(RETRY_TABLE_SIZE) scan, ~µs).
 *
 * @param rt              Retry table.
 * @param tx_queue        TX ring-buffer.
 * @param pkt_id_counter  Monotonic counter (uint32_t); lower 16 bits used as
 *                        the new pkt_id.  Caller owns this counter.
 * @param now_ms          Current monotonic timestamp.
 * @return Number of frames pushed to tx_queue this call.
 */
uint8_t retry_table_tick(retry_table_t *rt,
                         rb_t          *tx_queue,
                         uint32_t      *pkt_id_counter,
                         uint32_t       now_ms);

/** @brief Return the number of valid (occupied) entries. */
uint8_t retry_table_count(const retry_table_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_RETRY_TABLE_H */
