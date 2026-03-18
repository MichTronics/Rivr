/**
 * @file  firmware_core/timebase_nrf52.c
 * @brief Monotonic ms timebase for nRF52840 (Arduino + FreeRTOS).
 *
 * Replaces timebase.c on nRF52 builds.  The Adafruit nRF52 Arduino BSP
 * includes FreeRTOS, so we use a one-shot-auto-reload software timer to
 * increment g_mono_ms every millisecond — keeping the same interface that
 * the rest of the codebase expects from timebase.h.
 *
 * tb_millis() (defined as an inline in timebase.h) reads g_mono_ms via
 * an atomic load, so it is safe from any context including the radio ISR.
 */

#if defined(RIVR_PLATFORM_NRF52840)

#include "timebase.h"
#include <FreeRTOS.h>
#include <timers.h>

/* ── Shared atomics (definition) ─────────────────────────────────────────── */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

/* ── Periodic 1 ms FreeRTOS software timer ───────────────────────────────── */

static TimerHandle_t s_mono_timer;

static void mono_timer_cb(TimerHandle_t t)
{
    (void)t;
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
}

void timebase_init(void)
{
    s_mono_timer = xTimerCreate(
        "mono_ms",
        pdMS_TO_TICKS(1),   /* period = 1 ms */
        pdTRUE,             /* auto-reload   */
        NULL,
        mono_timer_cb
    );
    if (s_mono_timer) {
        xTimerStart(s_mono_timer, 0);
    }
}

/* Alternative tick-hook path (not used on nRF52 but keeps the linker happy). */
void timebase_tick_hook(void)
{
    atomic_fetch_add_explicit(&g_mono_ms, portTICK_PERIOD_MS, memory_order_relaxed);
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

#endif /* RIVR_PLATFORM_NRF52840 */
