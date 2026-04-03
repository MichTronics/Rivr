/**
 * @file  firmware_core/timebase_rp2040.c
 * @brief Monotonic ms timebase for RP2040 Arduino builds.
 *
 * The RP2040 Arduino core does not provide FreeRTOS in this project, so the
 * monotonic clock is synchronized from `millis()` whenever the main loop or
 * compatibility delay helpers yield.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "timebase.h"
#include <Arduino.h>

atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

void timebase_init(void)
{
    atomic_store_explicit(&g_mono_ms, (uint32_t)millis(), memory_order_relaxed);
    atomic_store_explicit(&g_lmp_tick, 0u, memory_order_relaxed);
}

void timebase_tick_hook(void)
{
    atomic_store_explicit(&g_mono_ms, (uint32_t)millis(), memory_order_relaxed);
}

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
