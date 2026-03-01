/**
 * @file  timer_if.h
 * @brief Abstract timer / timebase HAL interface for RIVR firmware.
 *
 * PURPOSE
 * ───────
 * Provides a single consistent API for time queries throughout the firmware,
 * so that higher-level modules (routing, duty-cycle, metrics) do not need to
 * include platform-specific headers (freertos, esp_timer, etc.).
 *
 * Concrete implementation lives in timebase.c; this header is a thin
 * inline-wrapper layer that maps to `tb_millis()` / `tb_lmp_now()`.
 *
 * PORTING
 * ───────
 * To port RIVR to a different platform, provide an alternative
 * implementation for timer_if_millis() and optionally timer_if_ticks_us().
 * The rest of the firmware uses only the functions declared here.
 *
 * NOTE: Existing code that calls tb_millis() directly is still correct.
 * This header is an additive convenience layer.
 */

#pragma once
#include <stdint.h>
#include "../timebase.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Current monotonic millisecond counter since boot.
 *
 * 32-bit, wraps after ~49 days.  Safe to call from any task or ISR.
 * Maps to tb_millis() from timebase.h.
 */
static inline uint32_t timer_if_millis(void)
{
    return tb_millis();
}

/**
 * @brief Current Lamport logical tick.
 *
 * Incremented by the main loop each time a new mesh packet is accepted.
 * Used for causal ordering guarantees within a mesh session.
 * Maps to tb_lmp_now() from timebase.h.
 */
static inline uint32_t timer_if_lmp_tick(void)
{
    return tb_lmp_now();
}

/**
 * @brief Elapsed milliseconds since a recorded start time.
 *
 * Handles 32-bit wrap-around correctly.
 *
 * @param start_ms  Previously captured timer_if_millis() value.
 * @return          Milliseconds elapsed since @p start_ms.
 */
static inline uint32_t timer_if_elapsed(uint32_t start_ms)
{
    return timer_if_millis() - start_ms;   /* unsigned subtraction wraps OK */
}

/**
 * @brief Check whether a timeout has expired.
 *
 * @param start_ms   Start time from timer_if_millis().
 * @param timeout_ms Timeout duration in milliseconds.
 * @return true  → elapsed ≥ timeout_ms.
 * @return false → still within the timeout window.
 */
static inline bool timer_if_expired(uint32_t start_ms, uint32_t timeout_ms)
{
    return timer_if_elapsed(start_ms) >= timeout_ms;
}

#ifdef __cplusplus
}
#endif
