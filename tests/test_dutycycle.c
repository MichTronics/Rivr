/**
 * @file  test_dutycycle.c
 * @brief Host-native unit tests for the duty-cycle sliding-window limiter.
 *
 * Tests:
 *  TC-1  Single check+record: used_us equals toa_us
 *  TC-2  Budget gate: check() returns false when budget would be exceeded
 *  TC-3  Sliding window expiry: used_us drops when records age out
 *  TC-4  Buffer overflow (>DC_HISTORY_CAP inserts, no expiry):
 *          LRU eviction keeps used_us bounded instead of accumulating silently
 *  TC-5  Full recovery: after window passes, used_us returns to 0
 *
 * Build (via tests/Makefile from project root):
 *   make -C tests dutycycle
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

#include "dutycycle.h"
#include "rivr_metrics.h"

/* ── Test infrastructure ────────────────────────────────────────────────── */
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

/* g_rivr_metrics is defined in rivr_metrics.c (linked by Makefile) */
extern rivr_metrics_t g_rivr_metrics;

/* ── TC-1: Single check+record ────────────────────────────────────────────── */
static void tc1_single_record(void)
{
    puts("TC-1  Single check+record");
    dc_ctx_t dc;
    dutycycle_init(&dc);

    uint32_t now_ms = 1000u;
    uint32_t toa    = 200000u;   /* 200 ms */

    CHECK(dc.used_us == 0u, "used_us starts at 0");
    CHECK(dutycycle_check(&dc, now_ms, toa), "check passes on empty context");

    dutycycle_record(&dc, now_ms, toa);

    CHECK(dc.used_us == toa, "used_us equals recorded toa_us");
    CHECK(dutycycle_remaining_us(&dc) == DC_BUDGET_US - toa,
          "remaining_us = budget - toa");
}

/* ── TC-2: Budget gate ────────────────────────────────────────────────────── */
static void tc2_budget_gate(void)
{
    puts("TC-2  Budget gate");
    dc_ctx_t dc;
    dutycycle_init(&dc);

    uint32_t now_ms = 2000u;
    /* Fill used_us to exactly the budget */
    dc.used_us = DC_BUDGET_US;

    CHECK(!dutycycle_check(&dc, now_ms, 1u),
          "check blocked when used_us == budget");
    CHECK(g_rivr_metrics.duty_blocked == 1u,
          "duty_blocked metric incremented");

    /* Any remaining budget allows a smaller TX */
    dc.used_us = DC_BUDGET_US - 100u;
    CHECK(dutycycle_check(&dc, now_ms, 100u),
          "check passes when toa_us fits exactly");
    CHECK(!dutycycle_check(&dc, now_ms, 101u),
          "check blocked when toa_us exceeds remaining by 1");
}

/* ── TC-3: Sliding window expiry ─────────────────────────────────────────── */
static void tc3_window_expiry(void)
{
    puts("TC-3  Sliding window expiry");
    dc_ctx_t dc;
    dutycycle_init(&dc);

    uint32_t t0    = 500000u;    /* 500 s uptime */
    uint32_t toa   = 100000u;    /* 100 ms */

    dutycycle_record(&dc, t0, toa);
    CHECK(dc.used_us == toa, "record added to used_us");

    /* Advance time by exactly DC_WINDOW_MS — entry is NOT yet stale */
    uint32_t t1 = t0 + DC_WINDOW_MS;
    dutycycle_check(&dc, t1, 1u);    /* triggers expire_old */
    CHECK(dc.used_us == toa, "entry still in window at t0 + DC_WINDOW_MS");

    /* Advance by one more millisecond — entry is now stale */
    uint32_t t2 = t0 + DC_WINDOW_MS + 1u;
    dutycycle_check(&dc, t2, 1u);
    CHECK(dc.used_us == 0u, "used_us = 0 after entry ages out");
    CHECK(dutycycle_remaining_us(&dc) == DC_BUDGET_US,
          "full budget restored after expiry");
}

/* ── TC-4: Buffer overflow → LRU eviction keeps used_us bounded ────────── */
static void tc4_overflow_lru_eviction(void)
{
    puts("TC-4  Overflow: LRU eviction keeps used_us bounded");
    dc_ctx_t dc;
    dutycycle_init(&dc);

    /*
     * Insert DC_HISTORY_CAP + 10 records within the same window second so
     * expire_old never fires mid-test.  Each record has toa_us = 1 µs.
     *
     * Expected behaviour:
     *  - First DC_HISTORY_CAP records: used_us climbs to DC_HISTORY_CAP.
     *  - Records DC_HISTORY_CAP+1 … +10: LRU eviction removes the oldest
     *    entry (1 µs) then adds 1 µs → used_us stays at DC_HISTORY_CAP.
     *    Without the fix, used_us would keep climbing past DC_HISTORY_CAP.
     */
    uint32_t now_ms = 10000u;
    uint32_t toa    = 1u;
    uint32_t n      = DC_HISTORY_CAP + 10u;

    for (uint32_t i = 0; i < n; i++) {
        dutycycle_record(&dc, now_ms, toa);
    }

    uint64_t expected = (uint64_t)DC_HISTORY_CAP * toa;
    CHECK(dc.used_us == expected,
          "used_us == DC_HISTORY_CAP after overflow (LRU eviction)");
    CHECK(dc.used_us < DC_BUDGET_US,
          "used_us remains below DC_BUDGET_US");
}

/* ── TC-5: Full recovery after window passes ─────────────────────────────── */
static void tc5_full_recovery(void)
{
    puts("TC-5  Full recovery after window");
    dc_ctx_t dc;
    dutycycle_init(&dc);

    uint32_t t0  = 7200000u;  /* 2 h uptime */
    uint32_t toa = 50000u;    /* 50 ms */

    /* Record three TXs. */
    dutycycle_record(&dc, t0,          toa);
    dutycycle_record(&dc, t0 + 1000u,  toa);
    dutycycle_record(&dc, t0 + 2000u,  toa);
    CHECK(dc.used_us == 3u * toa, "three records accumulated");

    /* Advance past window: all three entries expired. */
    uint32_t t1 = t0 + DC_WINDOW_MS + 5000u;
    dutycycle_check(&dc, t1, 1u);

    CHECK(dc.used_us == 0u, "used_us returns to 0 after full window passes");
    CHECK(dutycycle_check(&dc, t1, (uint32_t)DC_BUDGET_US),
          "full budget available after recovery");
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    puts("══════════════════════════════════════════");
    puts("  Duty-cycle limiter tests");
    puts("══════════════════════════════════════════");

    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));

    tc1_single_record();
    tc2_budget_gate();
    tc3_window_expiry();
    tc4_overflow_lru_eviction();
    tc5_full_recovery();

    puts("══════════════════════════════════════════");
    printf("  PASS: %d   FAIL: %d\n", s_pass, s_fail);
    puts("══════════════════════════════════════════");

    return s_fail ? 1 : 0;
}
