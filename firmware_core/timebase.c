/**
 * @file  timebase.c
 * @brief Monotonic millis + Lamport logical-tick implementation.
 */

#include "timebase.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

/* ── Global atomics ─────────────────────────────────────────────────────── */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

/* ── Periodic timer callback (fires every 1 ms via esp_timer) ───────────── */
static void mono_timer_cb(void *arg)
{
    (void)arg;
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
}

static esp_timer_handle_t s_mono_timer;

void timebase_init(void)
{
    const esp_timer_create_args_t args = {
        .callback        = mono_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "mono_ms",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_mono_timer));
    /* Period = 1000 µs = 1 ms */
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_mono_timer, 1000));
}

void timebase_tick_hook(void)
{
    /* Alternative hook for CONFIG_FREERTOS_TICK_HOOK builds.
       Only used if esp_timer periodic is not available on the target. */
    atomic_fetch_add_explicit(&g_mono_ms, portTICK_PERIOD_MS, memory_order_relaxed);
}

uint32_t tb_lmp_advance(uint32_t sender_tick)
{
    uint32_t local;
    /* CAS loop: local = max(local, sender_tick) + 1 */
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
