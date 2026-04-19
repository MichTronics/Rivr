/**
 * @file  wasm_stubs.c
 * @brief Host/WASM stubs for symbols needed by the routing kernel that
 *        are normally provided by ESP-IDF, FreeRTOS, or radio drivers.
 *
 * Mirrors the approach in tests/test_stubs.c — satisfy the linker without
 * pulling in any platform-specific code.
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>

#include "timebase.h"
#include "ringbuf.h"
#include "radio_sx1262.h"

/* ── Timebase (FreeRTOS-free stubs) ─────────────────────────────────────── */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0);

void timebase_init(void) {}

void timebase_tick_hook(void)
{
    atomic_fetch_add_explicit(&g_mono_ms, 1u, memory_order_relaxed);
}

uint32_t tb_lmp_advance(uint32_t sender_tick)
{
    uint_fast32_t local;
    do {
        local = atomic_load_explicit(&g_lmp_tick, memory_order_relaxed);
        uint_fast32_t desired = (sender_tick > local
                                 ? (uint_fast32_t)sender_tick : local) + 1u;
        if (atomic_compare_exchange_weak_explicit(
                &g_lmp_tick, &local, desired,
                memory_order_release, memory_order_relaxed))
            return (uint32_t)desired;
    } while (1);
}

/* ── Radio ring-buffers (linker satisfaction) ────────────────────────────── */
static rf_rx_frame_t   _rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t _tx_storage[RF_TX_QUEUE_CAP];
rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

void radio_init_buffers_only(void)
{
    rb_init(&rf_rx_ringbuf, _rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   _tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));
}

void radio_init(void)     {}
void radio_start_rx(void) {}
bool radio_transmit(const rf_tx_request_t *r) { (void)r; return true; }
void radio_poll_rx(void)  {}
void IRAM_ATTR radio_isr(void *a) { (void)a; }
uint8_t radio_decode_frame(const rf_rx_frame_t *f, char *b, uint8_t n)
    { (void)f; (void)b; (void)n; return 0u; }
uint16_t radio_frame_sender_tick(const rf_rx_frame_t *f) { (void)f; return 0u; }

/* ── rivr_log (WASM: no serial output needed) ────────────────────────────── */
void rivr_log_emit(int level, const char *tag, const char *msg)
    { (void)level; (void)tag; (void)msg; }
