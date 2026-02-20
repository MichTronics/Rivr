/**
 * @file  test_stubs.c
 * @brief Host-build stubs for ESP-IDF / FreeRTOS symbols needed by the
 *        acceptance-test compilation unit.
 *
 * Compiled together with the production .c files so the linker is satisfied
 * without pulling in ESP-IDF.  Nothing here may be called from a real device.
 */

/* ESP-IDF attribute macros that have no meaning on the host */
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "timebase.h"
#include "ringbuf.h"
#include "radio_sx1262.h"

/* ── Timebase globals (normally defined in timebase.c which needs FreeRTOS) */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

void timebase_init(void) { /* no-op in test build */ }

void timebase_tick_hook(void)
{
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
}

uint32_t tb_lmp_advance(uint32_t sender_tick)
{
    uint32_t local;
    do {
        local = atomic_load_explicit(&g_lmp_tick, memory_order_relaxed);
        uint32_t desired = (sender_tick > local ? sender_tick : local) + 1u;
        if (atomic_compare_exchange_weak_explicit(
                &g_lmp_tick, &local, desired,
                memory_order_release, memory_order_relaxed))
            return desired;
    } while (1);
}

/* ── Test helper: advance the monotonic clock by delta_ms ─────────────────── */
void test_advance_ms(uint32_t delta_ms)
{
    atomic_fetch_add_explicit(&g_mono_ms, delta_ms, memory_order_relaxed);
}

/* ── Radio ring-buffer globals (normally in radio_sx1262.c) ───────────────── */
/* The acceptance test never calls radio_init_buffers_only(), but the linker
 * needs these symbols because pending_queue.h pulls in radio_sx1262.h which
 * declares them extern.                                                       */
static rf_rx_frame_t  _rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t _tx_storage[RF_TX_QUEUE_CAP];
rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

/* ── Stub for radio_init_buffers_only (used in sim mode; not called in test) */
void radio_init_buffers_only(void) { /* no-op */ }
void radio_init(void)     { /* no-op */ }
void radio_start_rx(void) { /* no-op */ }
bool radio_transmit(const rf_tx_request_t *r) { (void)r; return true; }
void radio_poll_rx(void)  { /* no-op */ }
void IRAM_ATTR radio_isr(void *a) { (void)a; }
uint8_t radio_decode_frame(const rf_rx_frame_t *f, char *b, uint8_t n)
    { (void)f; (void)b; (void)n; return 0u; }
uint16_t radio_frame_sender_tick(const rf_rx_frame_t *f) { (void)f; return 0u; }

/* Called once from test main() to initialise the global ring-buffers. */
void test_stubs_init(void)
{
    rb_init(&rf_rx_ringbuf, _rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   _tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));
    atomic_store_explicit(&g_mono_ms,  1000u, memory_order_relaxed); /* start at t=1s */
    atomic_store_explicit(&g_lmp_tick,    0u, memory_order_relaxed);
}
