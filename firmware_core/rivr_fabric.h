/**
 * @file  rivr_fabric.h
 * @brief RIVR Fabric — congestion-aware relay suppression for repeater nodes.
 *
 * OVERVIEW
 * ────────
 * Rivr Fabric is an optional congestion signal that sits *on top* of the
 * existing routing/dutycycle stack.  It NEVER changes the dedupe/TTL/budget
 * decisions made by routing_flood_forward(); it only adds an additional
 * DELAY or DROP on top for relay of PKT_CHAT / PKT_DATA when the local
 * channel / duty-cycle pressure is high.
 *
 * GUARD RAILS
 * ───────────
 * • PKT_ACK, PKT_ROUTE_REQ, PKT_ROUTE_RPL, PKT_BEACON, PKT_PROG_PUSH
 *   always get FABRIC_SEND_NOW regardless of score.
 * • Only PKT_CHAT and PKT_DATA relay is subject to fabric decisions.
 * • Enabled only when compiled with RIVR_FABRIC_REPEATER=1 (see below).
 *   With RIVR_FABRIC_REPEATER=0 all functions are no-ops.
 *
 * CONGESTION SCORE (0..100, integer-only)
 * ─────────────────────────────────────────
 * Uses a 60-second sliding window broken into 60 × 1-second buckets.
 * Each tick the current bucket is updated; stale buckets are zeroed.
 *
 *   score = clamp(
 *       (rx_per_s * 2) + (blocked_dc_per_s * 25) + (tx_fail_per_s * 10),
 *       0, 100)
 *
 * POLICY (RIVR_FABRIC_REPEATER=1, relay of CHAT/DATA only)
 * ──────────────────────────────────────────────────────────
 *   score >= 80  →  FABRIC_DROP
 *   score >= 50  →  FABRIC_DELAY  extra_delay = min(250 + (score-50)*10, 1000) ms
 *   score >= 20  →  FABRIC_DELAY  extra_delay = min((score-20)*10, 300) ms
 *   else         →  FABRIC_SEND_NOW
 *
 * TEST PLAN (see bottom of rivr_fabric.c)
 * ────────────────────────────────────────
 * Simulate duty-cycle blocks → blocked_dc_per_s rises → score rises:
 *   - At score 20..49 : relay CHAT/DATA delayed by up to 300 ms extra
 *   - At score 50..79 : relay CHAT/DATA delayed up to 1000 ms extra
 *   - At score >= 80  : relay CHAT/DATA dropped; look for log:
 *                        "FABRIC: drop relay pkt_type=N score=N"
 * ROUTE/ACK/BEACON must never appear in drop/delay logs.
 */

#ifndef RIVR_FABRIC_H
#define RIVR_FABRIC_H

#include <stdint.h>
#include "protocol.h"   /* rivr_pkt_hdr_t, PKT_* constants */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time enable/disable ─────────────────────────────────────────── */
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER 0
#endif

/* ── Decision enum ───────────────────────────────────────────────────────── */
typedef enum {
    FABRIC_SEND_NOW = 0,   /**< Forward immediately (or after base jitter)    */
    FABRIC_DELAY    = 1,   /**< Forward but add out_extra_delay_ms to due_ms  */
    FABRIC_DROP     = 2,   /**< Suppress relay entirely                       */
} fabric_decision_t;

/** Debug snapshot filled by rivr_fabric_get_debug().  All fields zero when
 *  RIVR_FABRIC_REPEATER == 0.  Embed directly in display_stats_t. */
typedef struct {
    uint8_t  score;               /**< Current congestion score 0..100       */
    uint16_t rx_per_s_x100;       /**< RX rate × 100  (e.g. 150 = 1.50/s)   */
    uint16_t blocked_per_s_x100;  /**< DC-blocked rate × 100                 */
    uint16_t fail_per_s_x100;     /**< TX-fail rate × 100                    */
    uint32_t relay_drop_total;    /**< Relays suppressed — lifetime counter   */
    uint32_t relay_delay_total;   /**< Relays delayed   — lifetime counter   */
} fabric_debug_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Initialise sliding-window state.  Call once at boot. */
void rivr_fabric_init(void);

/** Optional per-tick maintenance (currently a no-op; reserved for future use). */
void rivr_fabric_tick(uint32_t now_ms);

/** Call after every successfully decoded inbound RIVR frame. */
void rivr_fabric_on_rx(uint32_t now_ms, int8_t rssi_dbm, uint8_t len);

/** Call immediately before radio_transmit() (frame dequeued and about to TX). */
void rivr_fabric_on_tx_enqueued(uint32_t now_ms, uint32_t toa_us);

/** Call when radio_transmit() returns true. */
void rivr_fabric_on_tx_ok(uint32_t now_ms, uint32_t toa_us);

/** Call when radio_transmit() returns false. */
void rivr_fabric_on_tx_fail(uint32_t now_ms, uint32_t toa_us);

/** Call when dutycycle_check() returns false (TX blocked by duty-cycle). */
void rivr_fabric_on_tx_blocked_dc(uint32_t now_ms, uint32_t toa_us);

/**
 * @brief Decide whether a relay frame should be sent, delayed, or dropped.
 *
 * @param pkt              Decoded header of the packet being relayed.
 * @param now_ms           Current monotonic millisecond timestamp.
 * @param toa_us           Estimated Time-on-Air for this frame.
 * @param out_extra_delay_ms  Output: additional milliseconds to add to due_ms.
 *                            Always set (0 for SEND_NOW and DROP).
 *
 * @return  FABRIC_SEND_NOW, FABRIC_DELAY, or FABRIC_DROP.
 *
 * Hard rules (compiler cannot optimise away):
 *  • If RIVR_FABRIC_REPEATER == 0  → always FABRIC_SEND_NOW, *out_extra_delay_ms=0.
 *  • If pkt_type is not PKT_CHAT or PKT_DATA → always FABRIC_SEND_NOW.
 */
fabric_decision_t rivr_fabric_decide_relay(
    const rivr_pkt_hdr_t *pkt,
    uint32_t              now_ms,
    uint32_t              toa_us,
    uint32_t             *out_extra_delay_ms
);

/**
 * @brief  Fill a debug snapshot of current fabric state.
 * @param  now_ms  Current monotonic time (advances the sliding-window ring).
 * @param  out     Must not be NULL; filled on return.
 *
 * Cost: one 60-bucket sweep — safe to call every main-loop iteration.
 * With RIVR_FABRIC_REPEATER=0 all fields in @p out are zero.
 */
void rivr_fabric_get_debug(uint32_t now_ms, fabric_debug_t *out);

#ifdef __cplusplus
}
#endif
#endif /* RIVR_FABRIC_H */
