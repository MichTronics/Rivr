/**
 * @file  test_radio_recovery.c
 * @brief Host-native unit tests for SX1262 radio recovery logic.
 *
 * Tests fault-injection paths:
 *  TC-1  BUSY stuck → radio_busy_stall + radio_busy_timeout_total increment;
 *        streak=3 → hard reset; radio_reset_busy_stuck updated
 *  TC-2  Backoff blocks re-reset within RADIO_RESET_BACKOFF_MS (5 s)
 *  TC-3  Backoff expires after cooldown; reset fires again
 *  TC-4  RX silence ≥ RADIO_RX_SILENCE_MS → radio_rx_timeout increments only;
 *        no guarded reset is triggered
 *  TC-5  TX HW timeout × 3 streak → guarded reset; tx_timeout_total updated;
 *        radio_reset_tx_timeout updated
 *  TC-6  TX SW deadline exceeded → tx_deadline_total increments;
 *        tx_timeout_total stays zero (split counters don't overlap)
 *  TC-7  Per-reason reset counters are independent (busy_stuck vs tx_timeout)
 *
 * Build (via tests/Makefile from project root):
 *   make -C tests radio_recovery
 *
 * Exit code: 0 = all pass, 1 = at least one failure.
 *
 * IMPORTANT: compiled with -DRIVR_FAULT_INJECT=1 so fault control variables
 * (g_fault_busy_stuck, g_fault_tx_no_done, g_fault_rx_silence) and
 * radio_fault_reset_state() are available.
 */

/* Must precede any firmware header to silence the ESP attribute warnings */
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

/* Firmware headers — compiled together with radio_sx1262.c, rivr_metrics.c,
 * rivr_log.c by the Makefile so we get the real implementations. */
#include "radio_sx1262.h"
#include "rivr_metrics.h"
#include "timebase.h"
#include "platform_esp32.h"   /* platform function declarations */

/* ── Timebase globals (normally in timebase.c which needs FreeRTOS) ───────── */
atomic_uint_fast32_t g_mono_ms  = ATOMIC_VAR_INIT(1000u);  /* start at 1 s */
atomic_uint_fast32_t g_lmp_tick = ATOMIC_VAR_INIT(0u);

void timebase_init(void) { /* no-op */ }

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

static void tst_advance_ms(uint32_t delta_ms)
{
    atomic_fetch_add_explicit(&g_mono_ms, delta_ms, memory_order_relaxed);
}

/* ── Platform-hardware stubs (no real SPI/GPIO on host) ─────────────────── */
spi_device_handle_t g_spi_sx1262 = NULL;

/** When true, SPI responses for GetIrqStatus return HW-timeout (0x0200). */
static bool s_stub_spi_tx_timeout = false;
/**
 * When true, each GetIrqStatus SPI call inside the TX poll loop advances
 * g_mono_ms by 1 000 ms so the SW deadline (toa_us×2+100ms) is quickly
 * exceeded without triggering the HW-timeout IRQ flag.
 * Used to test the radio_transmit() "deadline exceeded" code path.
 */
static bool s_stub_advance_clock_on_getirq = false;

void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (rx) {
        memset(rx, 0, len);
        /* Simulate GetIrqStatus returning TX HW-timeout flag (0x0200).
         * GetIrqStatus: opcode=0x12, NOP, then 2 status bytes. len == 4. */
        if (s_stub_spi_tx_timeout && len == 4 && tx && tx[0] == 0x12) {
            rx[2] = 0x02;   /* high byte: bit 9 = 0x200 = Timeout */
            rx[3] = 0x00;
        }
        /* Advance the simulated clock on every GetIrqStatus poll so the TX
         * software deadline is exceeded on the next while-loop iteration.
         * Returns zeros for both IRQ bytes (no TxDone, no HW timeout). */
        if (s_stub_advance_clock_on_getirq && len == 4 && tx && tx[0] == 0x12) {
            atomic_fetch_add_explicit(&g_mono_ms, 1000u, memory_order_relaxed);
        }
    }
}

bool platform_sx1262_wait_busy(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;   /* normally succeeds; fault inject intercepts before this */
}

void platform_sx1262_reset(void)    { /* no-op */ }
void platform_sx1262_set_rxen(bool e) { (void)e; }

/* ── Minimal assertion framework ─────────────────────────────────────────── */
static unsigned s_pass = 0;
static unsigned s_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        s_fail++; \
    } else { \
        s_pass++; \
    } \
} while (0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Reset radio state + metrics between test cases. */
static void setup(void)
{
    radio_fault_reset_state();   /* clears all statics, fault flags, metrics */
    s_stub_spi_tx_timeout            = false;
    s_stub_advance_clock_on_getirq   = false;
    /* Start the clock forward so timestamps look "real". */
    atomic_store_explicit(&g_mono_ms, 5000u, memory_order_relaxed);
}

/* ── TC-1: BUSY stuck → stall counter + hard reset at streak 3 ──────────── */
static void tc1_busy_stuck_reset(void)
{
    printf("TC-1  BUSY stuck → stall+busy_timeout counters, reset at streak 3\n");
    setup();
    g_fault_busy_stuck = true;

    /* Make a minimal tx request */
    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x04;  /* RF_FRAME_CHAT */
    req.len     = 10u;
    req.toa_us  = 130000u;

    /* First stall */
    bool r = radio_transmit(&req);
    CHECK(r == false);
    CHECK(g_rivr_metrics.radio_busy_stall == 1u);
    CHECK(g_rivr_metrics.radio_busy_timeout_total == 1u);  /* Step-9 counter */
    CHECK(g_rivr_metrics.radio_hard_reset == 0u);

    /* Second stall */
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_busy_stall == 2u);
    CHECK(g_rivr_metrics.radio_busy_timeout_total == 2u);
    CHECK(g_rivr_metrics.radio_hard_reset == 0u);

    /* Third stall → streak=3 → radio_guard_reset(BUSY_STUCK) → hard reset */
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_busy_stall == 3u);
    CHECK(g_rivr_metrics.radio_busy_timeout_total == 3u);
    CHECK(g_rivr_metrics.radio_hard_reset == 1u);
    CHECK(g_rivr_metrics.radio_reset_busy_stuck == 1u);  /* per-reason counter */
    CHECK(g_rivr_metrics.radio_reset_tx_timeout  == 0u); /* other reason: zero */
    CHECK(g_rivr_metrics.radio_reset_backoff == 0u);
}

/* ── TC-2: Backoff blocks re-reset within RADIO_RESET_BACKOFF_MS ─────────── */
static void tc2_backoff_blocks_reset(void)
{
    printf("TC-2  Backoff blocks rapid re-reset within 5 s window\n");
    setup();

    /* First reset via TC-1 path (reuse same logic). */
    g_fault_busy_stuck = true;
    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x04; req.len = 10u; req.toa_us = 130000u;

    /* Burn through streak to trigger first reset */
    radio_transmit(&req);
    radio_transmit(&req);
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_hard_reset == 1u);

    /* Advance time by 4 999 ms (< 5 000 ms backoff) — guard must still deny */
    tst_advance_ms(4999u);

    /* Another 3-stall streak — guard must deny this reset */
    radio_transmit(&req);   /* streak=1 */
    radio_transmit(&req);   /* streak=2 */
    radio_transmit(&req);   /* streak=3 → guard fires, backoff → denied */

    CHECK(g_rivr_metrics.radio_hard_reset == 1u);     /* still only 1 */
    CHECK(g_rivr_metrics.radio_reset_backoff >= 1u);  /* backoff counted */
}

/* ── TC-3: Backoff expires; re-reset is allowed ──────────────────────────── */
static void tc3_backoff_expires(void)
{
    printf("TC-3  Backoff expires after 5 s cooldown\n");
    setup();

    g_fault_busy_stuck = true;
    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x04; req.len = 10u; req.toa_us = 130000u;

    /* Trigger first reset */
    radio_transmit(&req);
    radio_transmit(&req);
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_hard_reset == 1u);

    /* Advance past the full backoff cooldown (5 001 ms > 5 000 ms) */
    tst_advance_ms(5001u);

    /* Another 3-stall streak → guard should allow reset this time */
    radio_transmit(&req);
    radio_transmit(&req);
    radio_transmit(&req);

    CHECK(g_rivr_metrics.radio_hard_reset == 2u);
    printf("       radio_reset_backoff = %u (should be 0)\n",
           (unsigned)g_rivr_metrics.radio_reset_backoff);
    CHECK(g_rivr_metrics.radio_reset_backoff == 0u);
    CHECK(g_rivr_metrics.radio_reset_busy_stuck == 2u);  /* both resets same reason */
}

/* ── TC-4: RX silence timeout is observable but not fatal ───────────────── */
static void tc4_rx_silence_timeout(void)
{
    printf("TC-4  RX silence ≥ 60 s → radio_rx_timeout only; no guarded reset\n");
    setup();

    /* Start continuous RX (arms s_last_rx_event_ms = current time) */
    radio_start_rx();

    /* Advance just under threshold — no timeout yet */
    tst_advance_ms(59999u);
    radio_check_timeouts();
    CHECK(g_rivr_metrics.radio_rx_timeout == 0u);

    /* Advance 2 ms more (total ≥ 60 000 ms silence) */
    tst_advance_ms(2u);
    radio_check_timeouts();
    CHECK(g_rivr_metrics.radio_rx_timeout == 1u);
    CHECK(g_rivr_metrics.radio_hard_reset == 0u);        /* silence no longer resets */
    CHECK(g_rivr_metrics.radio_reset_rx_timeout == 0u);  /* compatibility counter stays zero */
    CHECK(g_rivr_metrics.radio_reset_busy_stuck == 0u);  /* other reasons: zero */

    /* Second call immediately after: s_last_rx_event_ms was reset → no re-trigger */
    radio_check_timeouts();
    CHECK(g_rivr_metrics.radio_rx_timeout == 1u);   /* unchanged */
}

/* ── TC-5: TX HW timeout × 3 streak → guarded reset ─────────────────────── */
static void tc5_tx_hw_timeout_reset(void)
{
    printf("TC-5  TX HW timeout × 3 streak → guarded reset; tx_timeout_total + radio_reset_tx_timeout\n");
    setup();

    /* Make SPI GetIrqStatus return the TX HW-timeout IRQ flag (0x0200).
     * This causes radio_transmit()'s inner while-loop to hit the
     * "if (flags & 0x0200) { Timeout }" branch immediately on the first
     * poll iteration — no need to advance the clock. */
    s_stub_spi_tx_timeout = true;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x04;
    req.len     = 10u;
    req.toa_us  = 130000u;

    /* TX fail 1 */
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_tx_fail    == 1u);
    CHECK(g_rivr_metrics.tx_timeout_total == 1u);   /* HW-timeout split counter */
    CHECK(g_rivr_metrics.tx_deadline_total == 0u);  /* SW-deadline: not this path */
    CHECK(g_rivr_metrics.radio_hard_reset == 0u);

    /* TX fail 2 */
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_tx_fail    == 2u);
    CHECK(g_rivr_metrics.tx_timeout_total == 2u);
    CHECK(g_rivr_metrics.radio_hard_reset == 0u);

    /* TX fail 3 → streak=3 → guarded reset */
    radio_transmit(&req);
    CHECK(g_rivr_metrics.radio_tx_fail       == 3u);
    CHECK(g_rivr_metrics.tx_timeout_total    == 3u);
    CHECK(g_rivr_metrics.radio_hard_reset    == 1u);
    CHECK(g_rivr_metrics.radio_reset_tx_timeout  == 1u);  /* per-reason counter */
    CHECK(g_rivr_metrics.radio_reset_busy_stuck  == 0u);  /* different reason: zero */

    s_stub_spi_tx_timeout = false;
}

/* ── TC-6: TX SW deadline exceeded → tx_deadline_total; tx_timeout_total=0 ── */
static void tc6_tx_sw_deadline_total(void)
{
    printf("TC-6  TX SW deadline exceeded → tx_deadline_total; tx_timeout_total stays zero\n");
    setup();

    /* Enable the clock-advancing stub: every GetIrqStatus SPI call inside the
     * TX poll loop bumps g_mono_ms by 1 000 ms.  The loop's while condition
     * "tb_millis() < deadline_ms" then fires false on the next iteration,
     * taking the "SW deadline exceeded" code path.  SPI returns 0x0000 for
     * the IRQ flags so neither TxDone (0x0001) nor HW-timeout (0x0200) fire. */
    s_stub_advance_clock_on_getirq = true;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x04;
    req.len     = 10u;
    req.toa_us  = 130000u;  /* deadline = t0 + 260 + 100 = t0 + 360 ms */

    bool r = radio_transmit(&req);
    CHECK(r == false);
    CHECK(g_rivr_metrics.tx_deadline_total == 1u);  /* SW-deadline counter */
    CHECK(g_rivr_metrics.tx_timeout_total  == 0u);  /* HW-timeout: not triggered */
    CHECK(g_rivr_metrics.radio_tx_fail     == 1u);  /* legacy aggregate still works */
    CHECK(g_rivr_metrics.radio_hard_reset  == 0u);  /* only 1 stall, streak < 3 */

    s_stub_advance_clock_on_getirq = false;
}

/* ── TC-7: per-reason reset counters are mutually exclusive ──────────────── */
static void tc7_per_reason_counters_independent(void)
{
    printf("TC-7  Per-reason reset counters don't bleed into each other\n");
    setup();

    /* ① trigger a busy_stuck reset */
    g_fault_busy_stuck = true;
    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x04; req.len = 10u; req.toa_us = 130000u;
    radio_transmit(&req);
    radio_transmit(&req);
    radio_transmit(&req);   /* streak=3 → busy_stuck reset */
    CHECK(g_rivr_metrics.radio_reset_busy_stuck   == 1u);
    CHECK(g_rivr_metrics.radio_reset_tx_timeout   == 0u);
    CHECK(g_rivr_metrics.radio_reset_spurious_irq == 0u);
    CHECK(g_rivr_metrics.radio_reset_rx_timeout   == 0u);

    /* ② allow backoff to expire, then trigger a tx_timeout reset */
    g_fault_busy_stuck    = false;
    s_stub_spi_tx_timeout = true;
    tst_advance_ms(5001u);   /* past 5 s backoff */
    radio_transmit(&req);    /* streak=1 */
    radio_transmit(&req);    /* streak=2 */
    radio_transmit(&req);    /* streak=3 → tx_timeout reset */
    CHECK(g_rivr_metrics.radio_reset_busy_stuck   == 1u);  /* unchanged */
    CHECK(g_rivr_metrics.radio_reset_tx_timeout   == 1u);  /* incremented */
    CHECK(g_rivr_metrics.radio_reset_spurious_irq == 0u);
    CHECK(g_rivr_metrics.radio_reset_rx_timeout   == 0u);
    CHECK(g_rivr_metrics.radio_hard_reset         == 2u);  /* 2 total resets */

    s_stub_spi_tx_timeout = false;
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("═══ RIVR radio recovery tests (RIVR_FAULT_INJECT) ═══\n\n");

    tc1_busy_stuck_reset();
    tc2_backoff_blocks_reset();
    tc3_backoff_expires();
    tc4_rx_silence_timeout();
    tc5_tx_hw_timeout_reset();
    tc6_tx_sw_deadline_total();
    tc7_per_reason_counters_independent();

    printf("\n── Results: %u pass  %u fail ──\n", s_pass, s_fail);
    return (s_fail > 0u) ? 1 : 0;
}
