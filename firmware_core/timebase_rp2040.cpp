/**
 * @file  firmware_core/timebase_rp2040.cpp
 * @brief Monotonic ms timebase for RP2040 (Arduino + arduino-pico framework).
 *
 * Replaces timebase.c on RP2040 builds.  arduino-pico does not include
 * FreeRTOS by default, so instead of a FreeRTOS software timer we use the
 * pico-sdk hardware repeating timer (add_repeating_timer_ms) which fires
 * on the Cortex-M0+ hardware alarm with microsecond precision (pico/time.h).
 *
 * tb_millis() reads g_mono_ms via an atomic load and is safe from any
 * context including the radio ISR.
 *
 * Only compiled when RIVR_PLATFORM_RP2040 is defined (set by config.h).
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "timebase.h"
#include <pico/time.h>        /* add_repeating_timer_ms, repeating_timer_t */
#include <stdatomic.h>

/* ── Shared atomics (definition) ─────────────────────────────────────────── */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

/* ── Pico hardware repeating timer ───────────────────────────────────────── */

static repeating_timer_t s_mono_timer;

/**
 * Callback fired every 1 ms by the pico hardware alarm.
 * Returning true keeps the timer repeating.
 */
static bool mono_timer_cb(repeating_timer_t *rt)
{
    (void)rt;
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
    return true; /* keep repeating */
}

void timebase_init(void)
{
    /*
     * add_repeating_timer_ms(-1, ...) fires every 1 ms.
     * Negative delay means the next fire is scheduled relative to when the
     * callback returns (avoids drift accumulation).
     */
    add_repeating_timer_ms(-1, mono_timer_cb, NULL, &s_mono_timer);
}

/* Unused on RP2040 — included to satisfy the linker if its symbol is pulled in. */
void timebase_tick_hook(void)
{
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
}

/* ── Lamport logical tick ─────────────────────────────────────────────────── */

uint32_t tb_lmp_advance(uint32_t sender_tick)
{
    uint32_t local;
    do {
        local = atomic_load_explicit(&g_lmp_tick, memory_order_relaxed);
        uint32_t desired = (sender_tick > local ? sender_tick : local) + 1u;
        if (atomic_compare_exchange_weak_explicit(
                &g_lmp_tick, &local, desired,
                memory_order_release, memory_order_relaxed)) {
            return desired;
        }
    } while (1);
}

#endif /* RIVR_PLATFORM_RP2040 */
