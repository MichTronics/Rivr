/**
 * @file  test_beacon.c
 * @brief Host-native unit tests for beacon_sched — the pure-logic beacon
 *        scheduling state machine introduced in the EU868-density hardening
 *        pass.
 *
 * Tests:
 *  TC-1  set_initial_tx seeds last_tx_ms and zeroes startup_count
 *  TC-2  First BEACON_STARTUP_COUNT ticks return BEACON_TX_STARTUP
 *  TC-3  After startup_count reaches BEACON_STARTUP_COUNT, mode is normal
 *  TC-4  BEACON_STARTUP_WINDOW_MS forces startup_done even if burst unused
 *  TC-5  Neighbor-density suppression: count >= threshold → SUPPRESS_NEIGHBORS
 *  TC-6  Isolated node, interval elapsed → BEACON_TX_SCHEDULED
 *  TC-7  Interval gate: TX blocked when elapsed < interval (no jitter)
 *  TC-8  Jitter zero and exactly interval elapsed → TX fires on time
 *  TC-9  Jitter non-zero → next_min_tx_ms incorporates jitter offset
 *  TC-10 Dense-then-sparse transition: suppressed then fires after neighbor leaves
 *
 * Build (via tests/Makefile from project root):
 *   make -C tests beacon
 *
 * Exit code: 0 = all pass, 1 = at least one failure.
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "beacon_sched.h"

/* ── Test infrastructure ─────────────────────────────────────────────────── */

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(expr, msg)                                                       \
    do {                                                                       \
        if (expr) {                                                            \
            printf("  PASS  %s\n", msg);                                       \
            s_pass++;                                                          \
        } else {                                                               \
            printf("  FAIL  %s  (line %d)\n", msg, __LINE__);                 \
            s_fail++;                                                          \
        }                                                                     \
    } while (0)

/* ── TC-1: set_initial_tx seeds last_tx_ms ───────────────────────────────── */

static void tc1_set_initial_tx(void)
{
    puts("TC-1  set_initial_tx seeds state");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    uint32_t now = 5000u;
    beacon_sched_set_initial_tx(&s, now);

    CHECK(s.last_tx_ms == now, "last_tx_ms set to now");
    CHECK(s.startup_count == 0u, "startup_count still 0 after set_initial_tx");
    CHECK(s.startup_done == false, "startup_done still false after set_initial_tx");
}

/* ── TC-2: Startup burst fires BEACON_STARTUP_COUNT times ───────────────── */

static void tc2_startup_burst(void)
{
    puts("TC-2  Startup burst fires BEACON_STARTUP_COUNT times");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    /* All calls within the startup window and before any interval elapses */
    uint32_t now         = 1000u;
    uint32_t interval_ms = BEACON_STARTUP_WINDOW_MS / 2u;   /* 150 000 ms */
    uint32_t jitter      = 0u;
    uint32_t neighbors   = 0u;
    uint32_t entropy     = 0xDEADBEEFu;

    for (uint32_t i = 0; i < BEACON_STARTUP_COUNT; i++) {
        beacon_sched_result_t r = beacon_sched_tick(&s, now, interval_ms,
                                                    jitter, neighbors, entropy);
        CHECK(r == BEACON_TX_STARTUP, "startup tick returns BEACON_TX_STARTUP");
        CHECK(s.startup_count == (i + 1u), "startup_count incremented");
        now += 1000u;  /* small advance that is well within the window */
    }
    CHECK(s.startup_done == true, "startup_done set after burst exhausted");
}

/* ── TC-3: After startup, normal interval gate applies ──────────────────── */

static void tc3_after_startup_normal_interval(void)
{
    puts("TC-3  Normal interval gate after startup burst");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    uint32_t interval_ms = 300000u;
    uint32_t jitter      = 0u;
    uint32_t neighbors   = 0u;
    uint32_t entropy     = 0u;

    /* Exhaust the startup burst at t=1s, t=2s, … */
    uint32_t now = 1000u;
    for (uint32_t i = 0; i < BEACON_STARTUP_COUNT; i++) {
        beacon_sched_tick(&s, now, interval_ms, jitter, neighbors, entropy);
        now += 1000u;
    }

    /* Tick immediately after burst — interval_ms has NOT elapsed → suppress */
    beacon_sched_result_t r = beacon_sched_tick(&s, now, interval_ms,
                                                jitter, neighbors, entropy);
    CHECK(r == BEACON_SUPPRESS_INTERVAL,
          "SUPPRESS_INTERVAL right after startup burst");

    /* Advance past the full interval — TX should be allowed */
    now += interval_ms + 1u;
    r = beacon_sched_tick(&s, now, interval_ms, jitter, neighbors, entropy);
    CHECK(r == BEACON_TX_SCHEDULED,
          "BEACON_TX_SCHEDULED after interval elapsed");
}

/* ── TC-4: BEACON_STARTUP_WINDOW_MS forces startup_done ─────────────────── */

static void tc4_startup_window_timeout_forces_done(void)
{
    puts("TC-4  Window timeout forces startup_done");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    /* Tick exactly at the window boundary with zero startup used */
    uint32_t now         = BEACON_STARTUP_WINDOW_MS;
    uint32_t interval_ms = 600000u;
    uint32_t jitter      = 0u;
    uint32_t neighbors   = 0u;
    uint32_t entropy     = 42u;

    /* last_tx_ms == 0, elapsed is "infinite" → TX should happen (not startup) */
    beacon_sched_result_t r = beacon_sched_tick(&s, now, interval_ms,
                                                jitter, neighbors, entropy);
    CHECK(s.startup_done == true, "startup_done forced at window boundary");
    /* Should TX rather than suppress because last_tx_ms was 0 */
    CHECK(r == BEACON_TX_SCHEDULED,
          "BEACON_TX_SCHEDULED when window expired and interval met");
}

/* ── TC-5: Dense neighbor suppression ───────────────────────────────────── */

static void tc5_neighbor_suppression(void)
{
    puts("TC-5  Dense neighbor suppression");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    /* Force startup_done so we test the regular path */
    s.startup_done = true;

    uint32_t now         = BEACON_STARTUP_WINDOW_MS + 1000u;
    uint32_t interval_ms = 300000u;
    uint32_t jitter      = 0u;
    uint32_t entropy     = 0u;

    /* At or above threshold: suppressed */
    beacon_sched_result_t r = beacon_sched_tick(&s, now, interval_ms, jitter,
                                                BEACON_SUPPRESS_MIN_NEIGHBORS,
                                                entropy);
    CHECK(r == BEACON_SUPPRESS_NEIGHBORS,
          "SUPPRESS_NEIGHBORS at min_neighbors threshold");

    r = beacon_sched_tick(&s, now, interval_ms, jitter,
                          BEACON_SUPPRESS_MIN_NEIGHBORS + 5u, entropy);
    CHECK(r == BEACON_SUPPRESS_NEIGHBORS,
          "SUPPRESS_NEIGHBORS above threshold");
}

/* ── TC-6: Isolated node fires TX when interval elapsed ─────────────────── */

static void tc6_isolated_tx(void)
{
    puts("TC-6  Isolated node fires TX when interval elapsed");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    s.startup_done   = true;
    s.last_tx_ms     = 1000u;
    s.next_min_tx_ms = 300000u;  /* interval, no jitter */

    uint32_t now     = 1000u + 300000u + 1u;   /* just past next_min_tx_ms */
    uint32_t interval = 300000u;
    uint32_t jitter   = 0u;
    uint32_t entropy  = 0u;

    beacon_sched_result_t r = beacon_sched_tick(&s, now, interval, jitter, 0u, entropy);
    CHECK(r == BEACON_TX_SCHEDULED, "TX fires when interval elapsed, 0 neighbors");
}

/* ── TC-7: Interval gate blocks TX before elapsed ───────────────────────── */

static void tc7_interval_gate(void)
{
    puts("TC-7  Interval gate blocks TX before elapsed");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    s.startup_done   = true;
    s.last_tx_ms     = 1000u;
    s.next_min_tx_ms = 300000u;

    /* Now is only 100 s past last TX — interval not yet elapsed */
    uint32_t now     = 1000u + 100000u;
    uint32_t interval = 300000u;

    beacon_sched_result_t r = beacon_sched_tick(&s, now, interval, 0u, 0u, 0u);
    CHECK(r == BEACON_SUPPRESS_INTERVAL,
          "SUPPRESS_INTERVAL before next_min_tx_ms");
}

/* ── TC-8: Jitter zero — exactly interval_ms enforced ───────────────────── */

static void tc8_jitter_zero(void)
{
    puts("TC-8  Jitter zero — exact interval enforced");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    s.startup_done = true;

    uint32_t now      = BEACON_STARTUP_WINDOW_MS + 1u;
    uint32_t interval = 300000u;
    uint32_t entropy  = 0x12345678u;   /* any value — jitter_max=0 → no randomness */

    beacon_sched_result_t r = beacon_sched_tick(&s, now, interval, 0u, 0u, entropy);
    CHECK(r == BEACON_TX_SCHEDULED, "TX fires (last_tx_ms was 0)");

    /* next_min_tx_ms should be exactly interval (no jitter) */
    CHECK(s.next_min_tx_ms == interval, "next_min_tx_ms == interval when jitter=0");
}

/* ── TC-9: Non-zero jitter extends next_min_tx_ms ───────────────────────── */

static void tc9_jitter_nonzero(void)
{
    puts("TC-9  Non-zero jitter extends next_min_tx_ms");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    s.startup_done = true;

    uint32_t now       = BEACON_STARTUP_WINDOW_MS + 1u;
    uint32_t interval  = 300000u;
    uint32_t jitter    = 60000u;
    uint32_t entropy_a = 1u;
    uint32_t entropy_b = 0xFFFFFFFFu;

    /* First TX: record resulting next_min_tx_ms */
    beacon_sched_tick(&s, now, interval, jitter, 0u, entropy_a);
    uint32_t next_a = s.next_min_tx_ms;

    /* Reset and fire again with different entropy to check it changes */
    memset(&s, 0, sizeof(s));
    s.startup_done = true;
    beacon_sched_tick(&s, now, interval, jitter, 0u, entropy_b);
    uint32_t next_b = s.next_min_tx_ms;

    CHECK(next_a >= interval, "next_min_tx_ms >= interval");
    CHECK(next_a <= interval + jitter, "next_min_tx_ms <= interval + jitter");
    CHECK(next_b >= interval, "second next_min_tx_ms >= interval");
    CHECK(next_a != next_b || entropy_a == entropy_b,
          "different entropy yields different next_min_tx_ms (or same if equal)");
    /* With entropy 1 vs 0xFFFFFFFF modulo 60000, they should differ */
    CHECK(next_a != next_b, "jitter actually produces different offsets");
}

/* ── TC-10: Dense then sparse: suppressed then fires ────────────────────── */

static void tc10_dense_then_sparse(void)
{
    puts("TC-10  Dense then sparse: suppressed then TX fires");
    beacon_sched_t s;
    memset(&s, 0, sizeof(s));

    s.startup_done = true;

    uint32_t interval = 300000u;
    uint32_t now      = BEACON_STARTUP_WINDOW_MS + 1u;

    /* Dense: 2 ticks suppressed */
    for (int i = 0; i < 2; i++) {
        beacon_sched_result_t r = beacon_sched_tick(
            &s, now + (uint32_t)i * 60000u, interval, 0u,
            BEACON_SUPPRESS_MIN_NEIGHBORS, 0u);
        CHECK(r == BEACON_SUPPRESS_NEIGHBORS, "suppressed while dense");
    }

    /* Sparse: neighbor count drops to 1 (below threshold), interval elapsed */
    /* last_tx_ms was never set — TX fires */
    beacon_sched_result_t r = beacon_sched_tick(
        &s, now + interval + 1u, interval, 0u, 1u, 0u);
    CHECK(r == BEACON_TX_SCHEDULED, "TX fires once below neighbor threshold");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    puts("=== beacon_sched unit tests ===");
    tc1_set_initial_tx();
    tc2_startup_burst();
    tc3_after_startup_normal_interval();
    tc4_startup_window_timeout_forces_done();
    tc5_neighbor_suppression();
    tc6_isolated_tx();
    tc7_interval_gate();
    tc8_jitter_zero();
    tc9_jitter_nonzero();
    tc10_dense_then_sparse();

    printf("\n%d passed, %d failed\n", s_pass, s_fail);
    return (s_fail != 0) ? 1 : 0;
}
