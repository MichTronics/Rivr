/**
 * @file  test_replay.c
 * @brief CI-friendly deterministic replay test runner.
 *
 * Runs three JSONL trace scenarios through the firmware C-layer replay harness:
 *
 *  SCENARIO A — Multi-hop forward
 *    Injects three nodes exchanging CHAT messages.  Verifies flood-forward,
 *    deduplication, neighbour-table population, and route-cache learning.
 *    No fault injection; represents the nominal mesh-relay happy path.
 *
 *  SCENARIO B — Saturation (token bucket + queue overflow)
 *    Drains the global airtime token bucket via fault injection, then sends
 *    CHAT frames that should be dropped (class_drops_chat).
 *    Also pre-fills rf_tx_queue and verifies tx_queue_full increments.
 *
 *  SCENARIO C — Route aging + fault injection
 *    NODE_A is seen once; clock is advanced past NEIGHBOR_EXPIRY_MS and
 *    RCACHE_EXPIRY_MS.  Verifies that neighbour count drops to 0 and the
 *    route is absent.  Also exercises:
 *      • crc_fail burst     → radio_rx_crc_fail + rx_decode_fail
 *      • busy_stuck burst   → radio_busy_stall
 *
 * Build via tests/Makefile:
 *   make -C tests replay   (or: make -C tests  which builds all suites)
 *
 * Exit code: 0 = all assertions passed, 1 = at least one failure.
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "replay.h"
#include "rivr_metrics.h"

/* ── External stubs (test_stubs.c) ─────────────────────────────────────────── */
extern void test_stubs_init(void);
extern void test_set_ms(uint32_t abs_ms);

/* ── Trace file paths ────────────────────────────────────────────────────────── *
 * The test binary is executed from the tests/ directory, so traces/ is a     *
 * relative path from there.                                                  */
#ifndef TRACES_DIR
#  define TRACES_DIR   "traces/"
#endif

/* ── Runner ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    uint32_t total_pass = 0u;
    uint32_t total_fail = 0u;

    printf("\n═══ RIVR Replay Test Suite ═══════════════════════════════════\n");

    /* ── SCENARIO A: Multi-hop forward ───────────────────────────────────── */
    printf("\n[A] Multi-hop forward\n");
    test_stubs_init();
    replay_ctx_t a;
    replay_ctx_init(&a);
    replay_run_file(&a, TRACES_DIR "multihop.jsonl");
    total_pass += a.pass;
    total_fail += a.fail;

    /* ── SCENARIO B: Saturation ───────────────────────────────────────────── */
    printf("\n[B] Saturation (token bucket + queue overflow)\n");
    test_stubs_init();
    replay_ctx_t b;
    replay_ctx_init(&b);
    replay_run_file(&b, TRACES_DIR "saturation.jsonl");
    total_pass += b.pass;
    total_fail += b.fail;

    /* ── SCENARIO C: Route aging + fault injection ───────────────────────── */
    printf("\n[C] Route aging + fault injection\n");
    test_stubs_init();
    replay_ctx_t c;
    replay_ctx_init(&c);
    replay_run_file(&c, TRACES_DIR "route_aging.jsonl");
    total_pass += c.pass;
    total_fail += c.fail;

    /* ── Summary ──────────────────────────────────────────────────────────── */
    printf("\n══════════════════════════════════════════\n");
    printf("  PASS: %" PRIu32 "   FAIL: %" PRIu32 "\n", total_pass, total_fail);
    printf("══════════════════════════════════════════\n");

    return (total_fail > 0u) ? 1 : 0;
}
