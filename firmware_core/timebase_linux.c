/**
 * @file  timebase_linux.c
 * @brief Monotonic millisecond + Lamport logical-tick timebase for Linux.
 *
 * Replaces timebase.c and timebase_nrf52.c on Linux builds.
 *
 * g_mono_ms is advanced by a dedicated pthread that sleeps 1 ms and then
 * atomically increments the counter — matching the FreeRTOS/esp_timer
 * approach used on embedded targets.  tb_millis() (inline in timebase.h)
 * reads g_mono_ms via an atomic load so it is safe from any context.
 *
 * tb_lmp_advance() and tb_lmp_now() are identical to the ESP32/nRF52
 * implementations.
 */

#if defined(RIVR_PLATFORM_LINUX)

#include "timebase.h"
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

/* ── Shared atomic counters (definition) ─────────────────────────────────── */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

/* ── 1 ms timebase thread ────────────────────────────────────────────────── */

static pthread_t s_timebase_thread;

static void *timebase_thread_fn(void *arg)
{
    (void)arg;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };  /* 1 ms */
    while (1) {
        nanosleep(&ts, NULL);
        atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
    }
    return NULL;
}

void timebase_init(void)
{
    pthread_create(&s_timebase_thread, NULL, timebase_thread_fn, NULL);
}

/* Alternative tick-hook path (not used on Linux). */
void timebase_tick_hook(void)
{
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
}

/* ── Lamport logical tick ─────────────────────────────────────────────────── */

uint32_t tb_lmp_advance(uint32_t sender_tick)
{
    uint_fast32_t local;
    do {
        local = atomic_load_explicit(&g_lmp_tick, memory_order_relaxed);
        uint_fast32_t desired = (sender_tick > local ? sender_tick : local) + 1u;
        if (atomic_compare_exchange_weak_explicit(
                &g_lmp_tick, &local, desired,
                memory_order_release, memory_order_relaxed)) {
            return desired;
        }
    } while (1);
}

#endif /* RIVR_PLATFORM_LINUX */
