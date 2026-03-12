/**
 * @file  test_fabric.c
 * @brief Unit tests for the Rivr Fabric congestion engine (rivr_fabric.c).
 *
 * Tests every decision-threshold boundary defined in rivr_fabric.h:
 *
 *   score < 20                → FABRIC_SEND_NOW
 *   score = 20  (boundary)   → FABRIC_DELAY, extra = 0 ms          (light band entry)
 *   score = 49  (boundary-1) → FABRIC_DELAY, extra = 290 ms        (light band exit)
 *   score = 50  (boundary)   → FABRIC_DELAY, extra = 250 ms        (heavy band entry)
 *   score = 79  (boundary-1) → FABRIC_DELAY, extra = 540 ms        (not DROP)
 *   score = 80  (boundary)   → FABRIC_DROP                         (drop entry)
 *   score = 94  (boundary-1) → FABRIC_DROP                         (not blackout guard)
 *   score = 95  (boundary)   → FABRIC_DELAY, extra = 1000 ms       (blackout guard)
 *   control pkt, any score   → FABRIC_SEND_NOW                     (guard rule)
 *
 * Score is driven exclusively via rivr_fabric_on_tx_blocked_dc().
 *   score ≈ blocked_total * 25 / 60  (integer, see scoring formula in rivr_fabric.c)
 *
 * Event counts for target scores (verified analytically):
 *   47  → score 19   (below LIGHT_DELAY threshold)
 *   48  → score 20   (LIGHT_DELAY threshold)
 *   119 → score 49   (just below DELAY threshold)
 *   120 → score 50   (DELAY threshold)
 *   191 → score 79   (just below DROP threshold)
 *   192 → score 80   (DROP threshold)
 *   227 → score 94   (just below BLACKOUT_GUARD threshold)
 *   228 → score 95   (BLACKOUT_GUARD threshold)
 *
 * All events are injected at now_ms=500 (within second 0) so they accumulate
 * in a single sliding-window bucket, keeping the arithmetic predictable.
 *
 * Build (from project root):
 *   gcc -O2 -std=c11 \
 *       -I. -Ifirmware_core -Itests/include \
 *       -DRIVR_FABRIC_REPEATER=1 -DIRAM_ATTR="" \
 *       firmware_core/rivr_fabric.c \
 *       firmware_core/rivr_metrics.c \
 *       firmware_core/rivr_log.c \
 *       tests/test_fabric.c \
 *       -o /tmp/test_fabric && /tmp/test_fabric
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "rivr_fabric.h"   /* fabric_decision_t, rivr_fabric_* API     */
#include "protocol.h"      /* PKT_CHAT, PKT_DATA, PKT_BEACON, rivr_pkt_hdr_t */

/* ── Minimal assertion framework ────────────────────────────────────────── */
static uint32_t s_pass = 0;
static uint32_t s_fail = 0;

#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }          \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg),              \
                        __FILE__, __LINE__); s_fail++; }            \
} while (0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Build a minimal PKT_CHAT header. */
static rivr_pkt_hdr_t make_chat(void)
{
    rivr_pkt_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.pkt_type = PKT_CHAT;
    h.src_id   = 0xDEADBEEFu;
    return h;
}

/** Build a minimal PKT_DATA header. */
static rivr_pkt_hdr_t make_data(void)
{
    rivr_pkt_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.pkt_type = PKT_DATA;
    h.src_id   = 0xCAFEBABEu;
    return h;
}

/**
 * Reset fabric state then inject @p n DC-block events, all within second 0
 * (now_ms = 500ms).  Returns the decision for a PKT_CHAT relay at the same
 * timestamp, and writes the extra-delay value to *delay_out.
 */
static fabric_decision_t inject_and_decide(uint32_t n, uint32_t *delay_out,
                                           const rivr_pkt_hdr_t *pkt)
{
    rivr_fabric_init();
    for (uint32_t i = 0u; i < n; i++) {
        rivr_fabric_on_tx_blocked_dc(500u, 0u);
    }
    *delay_out = 0u;
    return rivr_fabric_decide_relay(pkt, 500u, 0u, delay_out);
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 1 – score < 20: SEND_NOW  (score = 19, blocked_total = 47)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_send_now_below_threshold(void)
{
    printf("\n=== TEST 1: score 19 → SEND_NOW ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(47u, &delay, &pkt);

    CHECK(d == FABRIC_SEND_NOW,     "score=19 → FABRIC_SEND_NOW");
    CHECK(delay == 0u,              "score=19 → extra_delay=0");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 2 – score = 20: DELAY entry, extra = 0 ms  (blocked_total = 48)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_light_delay_entry(void)
{
    printf("\n=== TEST 2: score 20 → DELAY (light, extra=0) ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(48u, &delay, &pkt);

    CHECK(d == FABRIC_DELAY,        "score=20 → FABRIC_DELAY");
    CHECK(delay == 0u,              "score=20 → extra_delay=0 ms");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 3 – score = 49: still DELAY (light band), extra = 290 ms
 *          (blocked_total = 119)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_light_delay_exit(void)
{
    printf("\n=== TEST 3: score 49 → DELAY (light, extra=290) ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(119u, &delay, &pkt);

    CHECK(d == FABRIC_DELAY,        "score=49 → FABRIC_DELAY (not DROP)");
    CHECK(delay == 290u,            "score=49 → extra_delay=290 ms");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 4 – score = 50: DELAY entry (heavy band), extra = 250 ms
 *          (blocked_total = 120)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_heavy_delay_entry(void)
{
    printf("\n=== TEST 4: score 50 → DELAY (heavy, extra=250) ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(120u, &delay, &pkt);

    CHECK(d == FABRIC_DELAY,        "score=50 → FABRIC_DELAY (not DROP)");
    CHECK(delay == 250u,            "score=50 → extra_delay=250 ms");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 5 – score = 79: still DELAY (heavy band, one below DROP), extra = 540 ms
 *          (blocked_total = 191)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_heavy_delay_boundary(void)
{
    printf("\n=== TEST 5: score 79 → DELAY (heavy, extra=540, not DROP) ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(191u, &delay, &pkt);

    CHECK(d == FABRIC_DELAY,        "score=79 → FABRIC_DELAY (not DROP)");
    CHECK(delay == 540u,            "score=79 → extra_delay=540 ms");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 6 – score = 80: first DROP  (blocked_total = 192)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_drop_entry(void)
{
    printf("\n=== TEST 6: score 80 → FABRIC_DROP ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(192u, &delay, &pkt);

    CHECK(d == FABRIC_DROP,         "score=80 → FABRIC_DROP");

    /* PKT_DATA also dropped at score 80 */
    rivr_pkt_hdr_t data_pkt = make_data();
    d = inject_and_decide(192u, &delay, &data_pkt);
    CHECK(d == FABRIC_DROP,         "score=80 PKT_DATA → FABRIC_DROP");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 7 – score = 94: still DROP, one below blackout guard
 *          (blocked_total = 227)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_drop_below_blackout_guard(void)
{
    printf("\n=== TEST 7: score 94 → FABRIC_DROP (not blackout guard) ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(227u, &delay, &pkt);

    CHECK(d == FABRIC_DROP,         "score=94 → FABRIC_DROP (< 95)");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 8 – score = 95: blackout guard converts DROP to DELAY(1000 ms)
 *          (blocked_total = 228)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_blackout_guard(void)
{
    printf("\n=== TEST 8: score 95 → DELAY (blackout guard, extra=1000) ===\n");
    rivr_pkt_hdr_t pkt  = make_chat();
    uint32_t       delay = 0xFFFFFFFFu;
    fabric_decision_t d  = inject_and_decide(228u, &delay, &pkt);

    CHECK(d == FABRIC_DELAY,        "score=95 → FABRIC_DELAY (blackout guard, not DROP)");
    CHECK(delay == 1000u,           "score=95 → extra_delay=1000 ms");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 9 – Control packets are NEVER delayed/dropped regardless of score
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_control_always_send_now(void)
{
    printf("\n=== TEST 9: control packets always SEND_NOW ===\n");

    const uint8_t ctrl_types[] = {
        PKT_BEACON, PKT_ROUTE_REQ, PKT_ROUTE_RPL, PKT_ACK, PKT_PROG_PUSH
    };

    /* Saturate the fabric to DROP score */
    rivr_fabric_init();
    for (uint32_t i = 0u; i < 228u; i++) {   /* score = 95, blackout guard */
        rivr_fabric_on_tx_blocked_dc(500u, 0u);
    }

    for (size_t t = 0; t < sizeof(ctrl_types); t++) {
        rivr_pkt_hdr_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.pkt_type = ctrl_types[t];
        pkt.src_id   = 0xABCDEF01u;

        uint32_t delay = 0xFFFFFFFFu;
        fabric_decision_t d = rivr_fabric_decide_relay(&pkt, 500u, 0u, &delay);

        char buf[64];
        snprintf(buf, sizeof(buf), "pkt_type=%u → FABRIC_SEND_NOW (control guard)",
                 (unsigned)ctrl_types[t]);
        CHECK(d == FABRIC_SEND_NOW, buf);
    }
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 10 – Counters are accurate after mix of decisions
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_counters(void)
{
    printf("\n=== TEST 10: lifetime relay counters ===\n");

    rivr_fabric_init();
    uint32_t drop_base  = rivr_fabric_get_relay_dropped();
    uint32_t delay_base = rivr_fabric_get_relay_delayed();
    uint32_t total_base = rivr_fabric_get_relay_total();

    /* Push 192 DC-block events → score=80 → DROP */
    for (uint32_t i = 0u; i < 192u; i++) {
        rivr_fabric_on_tx_blocked_dc(500u, 0u);
    }

    rivr_pkt_hdr_t pkt = make_chat();
    uint32_t delay = 0u;

    /* Two DROPs at score=80 */
    rivr_fabric_decide_relay(&pkt, 500u, 0u, &delay);
    rivr_fabric_decide_relay(&pkt, 500u, 0u, &delay);

    CHECK(rivr_fabric_get_relay_dropped() - drop_base == 2u,
          "dropped counter +2 after two DROPs");

    /* Change to score=20 (DELAY): reinit + 48 blocked */
    rivr_fabric_init();
    uint32_t delay_base2 = rivr_fabric_get_relay_delayed();
    uint32_t total_base2 = rivr_fabric_get_relay_total();
    for (uint32_t i = 0u; i < 48u; i++) {
        rivr_fabric_on_tx_blocked_dc(500u, 0u);
    }
    rivr_fabric_decide_relay(&pkt, 500u, 0u, &delay);
    rivr_fabric_decide_relay(&pkt, 500u, 0u, &delay);

    CHECK(rivr_fabric_get_relay_delayed() - delay_base2 == 2u,
          "delayed counter +2 after two DELAYs");
    CHECK(rivr_fabric_get_relay_total()   - total_base2 == 2u,
          "relay_total +2 (only CHAT/DATA counted)");

    (void)delay_base; (void)total_base;  /* used via delta logic above */
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * main
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== Rivr Fabric threshold tests (RIVR_FABRIC_REPEATER=1) ===\n");

    test_send_now_below_threshold();
    test_light_delay_entry();
    test_light_delay_exit();
    test_heavy_delay_entry();
    test_heavy_delay_boundary();
    test_drop_entry();
    test_drop_below_blackout_guard();
    test_blackout_guard();
    test_control_always_send_now();
    test_counters();

    printf("\n─────────────────────────────────────────────────────────\n");
    printf("Results: %lu passed, %lu failed\n",
           (unsigned long)s_pass, (unsigned long)s_fail);

    return (s_fail > 0u) ? 1 : 0;
}
