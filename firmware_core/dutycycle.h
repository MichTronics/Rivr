/**
 * @file  dutycycle.h
 * @brief Hard LoRa duty-cycle limiter (EU868: max 1% on sub-band, RIVR: 10%).
 *
 * POLICY
 * ──────
 *  • Tracks cumulative Time-on-Air (ToA) spent in the last `window_ms`
 *    milliseconds using a sliding-window accumulator.
 *  • `dutycycle_check()` returns false when the remaining budget would be
 *    exhausted, preventing the TX from being issued.
 *  • Budget is tracked purely in microseconds (no floats in hot path).
 *  • Designed to mirror the `budget.toa_us` RIVR operator so both layers
 *    agree on the available budget; RIVR filters the pipeline, this limiter
 *    is the final hardware gate.
 *
 * NOTE: RIVR's budget.toa_us() already filters the pipeline upstream.
 * This C layer is the last-resort hardware guard — it should never trip
 * during normal operation if RIVR is configured correctly.
 */

#ifndef DUTYCYCLE_H
#define DUTYCYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── Configuration ────────────────────────────────────────────────────────── */

/**  Sliding window length in milliseconds (default 1 hour = 3,600,000 ms). */
#define DC_WINDOW_MS      3600000UL

/** Duty-cycle percentage × 10 (default 100 = 10.0%). */
#define DC_DUTY_PCT_X10   100u     /* 10.0 % */

/**
 * Maximum total ToA budget per window in microseconds.
 *   budget_us = DC_WINDOW_MS * (DC_DUTY_PCT_X10 / 1000.0) * 1000
 *             = 3600000 * 0.100 * 1000 = 360,000,000 µs
 */
#define DC_BUDGET_US      (DC_WINDOW_MS * (uint64_t)(DC_DUTY_PCT_X10) * 1000ULL / 1000ULL)

/** History ring length (power-of-2). Each slot = one TX record. */
#define DC_HISTORY_CAP    64u

/* ── TX history slot ──────────────────────────────────────────────────────── */
typedef struct {
    uint32_t tx_end_ms;  /**< tb_millis() when the TX completed              */
    uint32_t toa_us;     /**< Actual ToA consumed by this TX                 */
} dc_tx_record_t;

/* ── Duty-cycle context (one per radio / sub-band) ────────────────────────── */
typedef struct {
    dc_tx_record_t history[DC_HISTORY_CAP]; /**< Circular TX history          */
    uint32_t       head;                    /**< Write index (next free slot)  */
    uint64_t       used_us;                 /**< Running total in window       */
    uint64_t       total_blocked_us;        /**< Aggregate blocked ToA (diag)  */
    uint32_t       blocked_count;           /**< Number of blocked TX attempts */
} dc_ctx_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Initialise the duty-cycle context. Call once from non-ISR context. */
void dutycycle_init(dc_ctx_t *dc);

/**
 * @brief Check whether a TX of `toa_us` microseconds is within budget.
 *
 * Expires old entries that fall outside the sliding window, then checks
 * if `toa_us` fits within the remaining budget.
 *
 * Call BEFORE radio_transmit(). If returns false, discard the TX request.
 *
 * @param dc     duty-cycle context
 * @param now_ms current tb_millis()
 * @param toa_us estimated Time-on-Air of the pending TX
 * @return true  TX is allowed
 * @return false TX would violate duty-cycle; must be dropped or deferred
 */
bool dutycycle_check(dc_ctx_t *dc, uint32_t now_ms, uint32_t toa_us);

/**
 * @brief Record a completed TX.
 *
 * Call AFTER radio_transmit() returns true to account for the time used.
 *
 * @param dc     duty-cycle context
 * @param now_ms tb_millis() at TX completion
 * @param toa_us actual Time-on-Air consumed
 */
void dutycycle_record(dc_ctx_t *dc, uint32_t now_ms, uint32_t toa_us);

/**
 * @brief Return remaining budget in microseconds for the current window.
 * Cheap read – does NOT expire old entries. Use for diagnostics only.
 */
uint64_t dutycycle_remaining_us(const dc_ctx_t *dc);

/** Print duty-cycle statistics to the log sink. */
void dutycycle_print_stats(const dc_ctx_t *dc);

/* ── Global context (single radio) ───────────────────────────────────────── */
extern dc_ctx_t g_dc;

#ifdef __cplusplus
}
#endif

#endif /* DUTYCYCLE_H */
