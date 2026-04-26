/**
 * @file  send_queue.h
 * @brief Originated-message outbox queue: serialises CLI-originated frames
 *        into rf_tx_queue at a rate the radio can actually sustain.
 *
 * PROBLEM SOLVED
 * ──────────────
 * rf_tx_queue holds only RF_TX_QUEUE_CAP (4) frames.  When a user sends
 * several messages in rapid succession the 5th frame is silently dropped
 * because rb_try_push() fails on a full ring.  This queue sits in front of
 * rf_tx_queue as a deep, expiry-aware FIFO that absorbs bursts.
 *
 * MECHANISM
 * ─────────
 * 1. cli_enqueue_chat() / cli_enqueue_chan_chat() call send_queue_enqueue()
 *    instead of pushing directly to rf_tx_queue.
 * 2. send_queue_tick() is called every main-loop iteration.  It pushes the
 *    head entry into rf_tx_queue whenever there is room.  At most one frame
 *    per call to keep the call bounded.
 * 3. Entries older than SEND_QUEUE_EXPIRY_MS are silently evicted so the
 *    queue never accumulates stale messages across e.g. a channel change.
 *
 * DELIVERY GUARANTEE (broadcast CHAT)
 * ────────────────────────────────────
 * Broadcast frames (dst_id == 0) have no ACK at the RF layer.  This queue
 * guarantees that every originated frame reaches the radio transmitter; it
 * cannot guarantee over-the-air reception.  For unicast ACK-based delivery
 * the retry_table layer (retry_table.h) provides end-to-end confirmation.
 *
 * MEMORY
 * ──────
 * SEND_QUEUE_SIZE × (RF_MAX_PAYLOAD_LEN + ~12 B) ≈ 4.2 kB in BSS.
 * Well within the ESP32's 320 kB DRAM budget.
 */

#ifndef RIVR_SEND_QUEUE_H
#define RIVR_SEND_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "radio_sx1262.h"   /* RF_MAX_PAYLOAD_LEN, rf_tx_request_t, rb_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */

/** FIFO depth — number of originated frames that can be buffered. */
#ifndef SEND_QUEUE_SIZE
#  define SEND_QUEUE_SIZE     16u
#endif

/** Evict unsent frames older than this many milliseconds (2 minutes). */
#ifndef SEND_QUEUE_EXPIRY_MS
#  define SEND_QUEUE_EXPIRY_MS  120000u
#endif

/* ── Types ───────────────────────────────────────────────────────────────── */

/** One slot in the send outbox. */
typedef struct {
    uint8_t  data[RF_MAX_PAYLOAD_LEN]; /**< Wire-encoded frame                */
    uint8_t  len;                      /**< Byte count in data[]              */
    uint32_t toa_us;                   /**< Time-on-Air estimate (µs)         */
    uint32_t enqueued_ms;              /**< tb_millis() at enqueue time       */
    bool     valid;                    /**< Slot occupied                     */
} send_entry_t;

/** Complete send-queue state (BSS-safe). */
typedef struct {
    send_entry_t entries[SEND_QUEUE_SIZE];
    uint8_t      head;   /**< Index of the oldest (next-to-send) entry       */
    uint8_t      tail;   /**< Index of the next free slot                    */
    uint8_t      count;  /**< Number of valid entries                        */
} send_queue_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Zero-initialise a send queue (call once at boot).
 */
void send_queue_init(send_queue_t *sq);

/**
 * @brief Append an originated frame to the outbox.
 *
 * Evicts entries older than SEND_QUEUE_EXPIRY_MS first to reclaim space.
 *
 * @param sq       Send queue state.
 * @param data     Wire-encoded frame bytes (from protocol_encode()).
 * @param len      Byte count of @p data (must be ≤ RF_MAX_PAYLOAD_LEN).
 * @param toa_us   Estimated Time-on-Air in microseconds.
 * @param now_ms   Current monotonic timestamp (tb_millis()).
 * @return true  → frame accepted.
 * @return false → queue full after expiry — frame dropped (counted in metrics).
 */
bool send_queue_enqueue(send_queue_t  *sq,
                        const uint8_t *data,
                        uint8_t        len,
                        uint32_t       toa_us,
                        uint32_t       now_ms);

/**
 * @brief Drain one frame from the outbox into tx_queue when there is room.
 *
 * Called every main-loop iteration.  Evicts expired head entries before
 * attempting a push.  Pushes at most one frame per call to bound latency.
 *
 * @param sq        Send queue state.
 * @param tx_queue  RF TX ring buffer (rf_tx_queue).
 * @param now_ms    Current monotonic timestamp.
 * @return Number of frames pushed (0 or 1).
 */
uint8_t send_queue_tick(send_queue_t *sq,
                        rb_t         *tx_queue,
                        uint32_t      now_ms);

/**
 * @brief Return the number of valid (non-expired) entries currently queued.
 */
static inline uint8_t send_queue_count(const send_queue_t *sq)
{
    return sq ? sq->count : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* RIVR_SEND_QUEUE_H */
