/**
 * @file  test_retry.c
 * @brief Unit-tests for the ACK / retry-table reliability layer.
 *
 * RUN 1 — Basic enqueue + immediate ACK
 *   Enqueue a minimal CHAT frame → count == 1.
 *   ack(src, pkt_id) → returns true, count drops to 0.
 *   Second ack() call → returns false (entry already gone).
 *
 * RUN 2 — Timeout expires → retry retransmit
 *   Enqueue → advance clock past RETRY_TIMEOUT_MS → tick().
 *   Frame pushed to TX queue with a fresh pkt_id in wire bytes [19–20].
 *   CRC-16 over the patched frame is still valid.
 *   Entry remains valid; retries_left decremented by 1.
 *
 * RUN 3 — ACK with post-retry pkt_id clears entry
 *   After one retry tick (new pkt_id in entry), ACK with the OLD pkt_id does
 *   not clear.  ACK with the NEW pkt_id clears the entry.
 *
 * RUN 4 — All retries exhausted → fallback flood
 *   Advance clock and tick four times (each with a doubling interval).
 *   After the fourth tick (retries_left == 0 when timer fires):
 *     • A fallback frame is pushed: flags has PKT_FLAG_FALLBACK, dst_id == 0.
 *     • Entry is cleared; retry_fail_total and retry_fallback_total increment.
 *
 * RUN 5 — Table-full rejection
 *   Enqueue RETRY_TABLE_SIZE (16) entries → all succeed.
 *   17th enqueue → returns false.
 *
 * RUN 6 — routing_build_ack produces a valid decodable frame
 *   routing_build_ack() → protocol_decode() succeeds; pkt_type == PKT_ACK;
 *   payload encodes correct ack_src_id and ack_pkt_id.
 *
 * Build:  see tests/Makefile target 'retry'.
 * Exit:   0 = all checks passed, 1 = at least one failure.
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

/* Firmware headers */
#include "protocol.h"
#include "routing.h"
#include "retry_table.h"
#include "radio_sx1262.h"
#include "timebase.h"
#include "rivr_metrics.h"

/* ── External stubs (test_stubs.c) ──────────────────────────────────────── */
extern void     test_stubs_init(void);
extern void     test_advance_ms(uint32_t delta_ms);
extern atomic_uint_fast32_t g_mono_ms;

/* TX queue is the shared global from test_stubs.c */
extern rb_t rf_tx_queue;

/* ── Node IDs ─────────────────────────────────────────────────────────────── */
#define MY_NODE  0xFEED0000ul
#define NODE_DST 0xBEEF0001ul
#define NEXT_HOP 0xAAAA0002ul

/* ── Assertion framework (mirrors test_acceptance.c) ────────────────────── */
static uint32_t s_pass = 0;
static uint32_t s_fail = 0;

#define CHECK(cond, msg) do {                                                  \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }                      \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg), __FILE__, __LINE__);     \
                 s_fail++; }                                                   \
} while (0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Build a minimal valid CHAT wire frame; returns frame length. */
static int build_frame(uint32_t src, uint32_t dst, uint16_t pkt_id,
                       uint8_t *out, uint8_t cap)
{
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_CHAT;
    hdr.flags       = 0u;
    hdr.ttl         = 7u;
    hdr.hop         = 0u;
    hdr.net_id      = 0u;
    hdr.src_id      = src;
    hdr.dst_id      = dst;
    hdr.seq         = 1u;
    hdr.pkt_id      = pkt_id;
    hdr.payload_len = 0u;
    return protocol_encode(&hdr, NULL, 0u, out, cap);
}

/** Drain all frames from rf_tx_queue into the provided array; returns count. */
static uint32_t drain_tx(rf_tx_request_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (n < max) {
        rf_tx_request_t r;
        if (!rb_pop(&rf_tx_queue, &r)) break;
        if (out) out[n] = r;
        n++;
    }
    return n;
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 1 — Basic enqueue + immediate ACK
 * ══════════════════════════════════════════════════════════════════════════ */
static void run1_enqueue_ack(void)
{
    printf("\n── RUN 1: enqueue + immediate ACK ─────────────────────────────\n");
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));

    retry_table_t rt;
    retry_table_init(&rt);

    uint8_t frame[RIVR_PKT_MIN_FRAME];
    int flen = build_frame(MY_NODE, NODE_DST, 0x0001u, frame, sizeof(frame));
    CHECK(flen == (int)RIVR_PKT_MIN_FRAME, "frame builds to RIVR_PKT_MIN_FRAME bytes");

    uint32_t now = tb_millis();

    /* Enqueue */
    bool ok = retry_table_enqueue(&rt, MY_NODE, 0x0001u,
                                   NODE_DST, NEXT_HOP,
                                   frame, (uint8_t)flen,
                                   1000u, now);
    CHECK(ok,                           "enqueue returns true");
    CHECK(retry_table_count(&rt) == 1u, "count == 1 after enqueue");

    /* Correct ACK clears the entry */
    bool acked = retry_table_ack(&rt, MY_NODE, 0x0001u);
    CHECK(acked,                        "ACK returns true for matching (src, pkt_id)");
    CHECK(retry_table_count(&rt) == 0u, "count == 0 after ACK");

    /* Wrong pkt_id → no match */
    retry_table_enqueue(&rt, MY_NODE, 0x0002u, NODE_DST, NEXT_HOP,
                        frame, (uint8_t)flen, 1000u, now);
    bool no_ack = retry_table_ack(&rt, MY_NODE, 0xDEADu);
    CHECK(!no_ack, "ACK with wrong pkt_id returns false");
    CHECK(retry_table_count(&rt) == 1u, "entry survives wrong-pkt_id ACK");

    /* Double-ACK returns false */
    retry_table_ack(&rt, MY_NODE, 0x0002u);
    bool second = retry_table_ack(&rt, MY_NODE, 0x0002u);
    CHECK(!second, "second ACK on same entry returns false");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 2 — Timeout expires → retry retransmit
 * ══════════════════════════════════════════════════════════════════════════ */
static void run2_timeout_retry(void)
{
    printf("\n── RUN 2: timeout → retry retransmit ──────────────────────────\n");
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));
    drain_tx(NULL, 64u);   /* clear any leftovers */

    retry_table_t rt;
    retry_table_init(&rt);

    uint8_t frame[RIVR_PKT_MIN_FRAME];
    int flen = build_frame(MY_NODE, NODE_DST, 0x0042u, frame, sizeof(frame));

    uint32_t now = tb_millis();
    retry_table_enqueue(&rt, MY_NODE, 0x0042u,
                        NODE_DST, NEXT_HOP,
                        frame, (uint8_t)flen,
                        1000u, now);

    uint32_t counter = 0x0042u;   /* matches the initial pkt_id */

    /* Tick before timeout fires — should NOT push any frame. */
    retry_table_tick(&rt, &rf_tx_queue, &counter, now + 100u);
    uint32_t pushed_early = drain_tx(NULL, 16u);
    CHECK(pushed_early == 0u, "tick before timeout does not retransmit");
    CHECK(retry_table_count(&rt) == 1u, "entry still valid before timeout");

    /* Advance past the initial timeout. */
    test_advance_ms(RETRY_TIMEOUT_MS + 1u);
    uint32_t t1 = tb_millis();
    uint8_t fired = retry_table_tick(&rt, &rf_tx_queue, &counter, t1);
    CHECK(fired == 1u, "exactly one frame pushed after timeout");

    /* Retrieve the pushed frame. */
    rf_tx_request_t popped;
    bool got = rb_pop(&rf_tx_queue, &popped);
    CHECK(got, "pop from TX queue succeeds");

    /* The pkt_id in the wire frame should be the updated counter value. */
    uint16_t wire_pkt_id = (uint16_t)(popped.data[PKT_ID_BYTE_OFFSET])
                         | ((uint16_t)(popped.data[PKT_ID_BYTE_OFFSET + 1u]) << 8u);
    uint16_t expected_pkt_id = (uint16_t)counter;   /* counter was incremented */
    CHECK(wire_pkt_id == expected_pkt_id, "wire pkt_id matches incremented counter");
    CHECK(wire_pkt_id != 0x0042u,         "retry pkt_id differs from original");

    /* Verify CRC is valid over the patched frame. */
    rivr_pkt_hdr_t decoded_hdr;
    const uint8_t *decoded_pl = NULL;
    bool crc_ok = protocol_decode(popped.data, popped.len, &decoded_hdr, &decoded_pl);
    CHECK(crc_ok, "CRC is valid on retransmitted frame");
    CHECK(decoded_hdr.pkt_id == wire_pkt_id, "decoded pkt_id matches patched wire bytes");

    /* Entry still present, retries_left has been decremented. */
    CHECK(retry_table_count(&rt) == 1u, "entry still valid after one retry");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 3 — ACK with post-retry pkt_id clears entry
 * ══════════════════════════════════════════════════════════════════════════ */
static void run3_ack_after_retry(void)
{
    printf("\n── RUN 3: ACK with new pkt_id after retry ──────────────────────\n");
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));
    drain_tx(NULL, 64u);

    retry_table_t rt;
    retry_table_init(&rt);

    uint8_t frame[RIVR_PKT_MIN_FRAME];
    build_frame(MY_NODE, NODE_DST, 0x0100u, frame, sizeof(frame));

    uint32_t now = tb_millis();
    retry_table_enqueue(&rt, MY_NODE, 0x0100u,
                        NODE_DST, NEXT_HOP,
                        frame, (uint8_t)sizeof(frame),
                        1000u, now);

    uint32_t counter = 0x0100u;

    /* Fire one retry. */
    test_advance_ms(RETRY_TIMEOUT_MS + 1u);
    retry_table_tick(&rt, &rf_tx_queue, &counter, tb_millis());
    uint16_t new_pkt_id = (uint16_t)counter;
    drain_tx(NULL, 16u);

    /* Old pkt_id should NOT clear the entry. */
    bool old_ack = retry_table_ack(&rt, MY_NODE, 0x0100u);
    CHECK(!old_ack,                         "ACK with old pkt_id returns false");
    CHECK(retry_table_count(&rt) == 1u,     "entry survives stale ACK");

    /* New pkt_id SHOULD clear it. */
    bool new_ack = retry_table_ack(&rt, MY_NODE, new_pkt_id);
    CHECK(new_ack,                          "ACK with new pkt_id returns true");
    CHECK(retry_table_count(&rt) == 0u,     "entry cleared after correct ACK");
    CHECK(g_rivr_metrics.retry_success_total == 0u,
          "retry_success_total not touched by retry_table_ack (incremented by caller)");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 4 — All retries exhausted → fallback flood
 * ══════════════════════════════════════════════════════════════════════════ */
static void run4_exhausted_fallback(void)
{
    printf("\n── RUN 4: exhausted → fallback flood ───────────────────────────\n");
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));
    drain_tx(NULL, 64u);

    retry_table_t rt;
    retry_table_init(&rt);

    uint8_t frame[RIVR_PKT_MIN_FRAME];
    build_frame(MY_NODE, NODE_DST, 0x0200u, frame, sizeof(frame));

    uint32_t now = tb_millis();
    retry_table_enqueue(&rt, MY_NODE, 0x0200u,
                        NODE_DST, NEXT_HOP,
                        frame, (uint8_t)sizeof(frame),
                        1000u, now);

    uint32_t counter = 0x0200u;

    /* Fire RETRY_MAX (3) retries — each doubles the interval. */
    uint32_t interval = RETRY_TIMEOUT_MS;
    for (uint8_t i = 0; i < RETRY_MAX; i++) {
        test_advance_ms(interval + 1u);
        uint8_t pushed = retry_table_tick(&rt, &rf_tx_queue, &counter, tb_millis());
        CHECK(pushed == 1u, "one frame pushed per retry");
        drain_tx(NULL, 4u);
        interval <<= 1u;   /* interval doubles each pass */
        CHECK(retry_table_count(&rt) == 1u, "entry valid during retry loop");
    }

    /* Now retries_left == 0; next tick should trigger fallback. */
    test_advance_ms(interval + 1u);
    uint8_t fb = retry_table_tick(&rt, &rf_tx_queue, &counter, tb_millis());
    CHECK(fb == 1u, "one fallback frame pushed after exhaustion");

    /* Retrieve the fallback frame. */
    rf_tx_request_t fallback;
    bool got = rb_pop(&rf_tx_queue, &fallback);
    CHECK(got, "fallback frame popped from TX queue");

    /* Validate fallback frame structure. */
    rivr_pkt_hdr_t fb_hdr;
    const uint8_t *fb_pl = NULL;
    bool fb_ok = protocol_decode(fallback.data, fallback.len, &fb_hdr, &fb_pl);
    CHECK(fb_ok,                                   "fallback frame CRC valid");
    CHECK(fb_hdr.dst_id == 0u,                     "fallback dst_id == 0 (broadcast)");
    CHECK(fb_hdr.ttl    == RIVR_FALLBACK_TTL,      "fallback TTL == RIVR_FALLBACK_TTL");
    CHECK((fb_hdr.flags & PKT_FLAG_FALLBACK) != 0, "PKT_FLAG_FALLBACK set");
    CHECK((fb_hdr.flags & PKT_FLAG_ACK_REQ)  == 0, "PKT_FLAG_ACK_REQ cleared");
    CHECK(fb_hdr.hop    == 0u,                     "fallback hop == 0");

    /* Metrics and entry cleanup. */
    CHECK(retry_table_count(&rt) == 0u,                 "entry cleared after fallback");
    CHECK(g_rivr_metrics.retry_fail_total     == 1u,    "retry_fail_total == 1");
    CHECK(g_rivr_metrics.retry_fallback_total == 1u,    "retry_fallback_total == 1");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 5 — Table full: 17th enqueue rejected
 * ══════════════════════════════════════════════════════════════════════════ */
static void run5_table_full(void)
{
    printf("\n── RUN 5: table full → 17th enqueue rejected ───────────────────\n");
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));

    retry_table_t rt;
    retry_table_init(&rt);

    uint8_t frame[RIVR_PKT_MIN_FRAME];
    build_frame(MY_NODE, NODE_DST, 0x0300u, frame, sizeof(frame));

    uint32_t now = tb_millis();

    for (uint16_t i = 0; i < RETRY_TABLE_SIZE; i++) {
        bool ok = retry_table_enqueue(&rt, MY_NODE, (uint16_t)(0x0300u + i),
                                       NODE_DST, NEXT_HOP,
                                       frame, (uint8_t)sizeof(frame),
                                       1000u, now);
        CHECK(ok, "enqueue within capacity succeeds");
    }

    CHECK(retry_table_count(&rt) == RETRY_TABLE_SIZE, "count == RETRY_TABLE_SIZE");

    bool overflow = retry_table_enqueue(&rt, MY_NODE, 0x9999u,
                                         NODE_DST, NEXT_HOP,
                                         frame, (uint8_t)sizeof(frame),
                                         1000u, now);
    CHECK(!overflow, "17th enqueue returns false (table full)");
    CHECK(retry_table_count(&rt) == RETRY_TABLE_SIZE, "count unchanged after rejected enqueue");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 6 — routing_build_ack produces a valid frame
 * ══════════════════════════════════════════════════════════════════════════ */
static void run6_routing_build_ack(void)
{
    printf("\n── RUN 6: routing_build_ack → valid decodable frame ────────────\n");

    uint8_t buf[RIVR_PKT_HDR_LEN + ACK_PAYLOAD_LEN + RIVR_PKT_CRC_LEN];
    int len = routing_build_ack(
        MY_NODE,        /* my_id (sender of ACK)         */
        NODE_DST,       /* dst_id (recipient of ACK)     */
        MY_NODE,        /* ack_src_id                    */
        0x1234u,        /* ack_pkt_id                    */
        0x0001u,        /* seq                           */
        0x0002u,        /* pkt_id                        */
        buf, (uint8_t)sizeof(buf));

    CHECK(len == (int)sizeof(buf), "routing_build_ack returns expected length");

    rivr_pkt_hdr_t hdr;
    const uint8_t *pl = NULL;
    bool ok = protocol_decode(buf, (uint8_t)len, &hdr, &pl);
    CHECK(ok,                      "routing_build_ack frame decodes (CRC valid)");
    CHECK(hdr.pkt_type == PKT_ACK, "pkt_type is PKT_ACK");
    CHECK(hdr.src_id   == MY_NODE, "src_id is my_id");
    CHECK(hdr.dst_id   == NODE_DST,"dst_id is destination");
    CHECK(hdr.payload_len == ACK_PAYLOAD_LEN, "payload_len == ACK_PAYLOAD_LEN");

    /* Validate payload bytes */
    CHECK(pl != NULL, "payload pointer is non-NULL");
    if (pl) {
        uint32_t got_src = (uint32_t)pl[0]
                         | ((uint32_t)pl[1] <<  8u)
                         | ((uint32_t)pl[2] << 16u)
                         | ((uint32_t)pl[3] << 24u);
        uint16_t got_pid = (uint16_t)pl[4] | ((uint16_t)pl[5] << 8u);
        CHECK(got_src == MY_NODE, "payload ack_src_id matches MY_NODE");
        CHECK(got_pid == 0x1234u, "payload ack_pkt_id matches 0x1234");
    }

    /* Buffer-overflow rejection */
    uint8_t small[4];
    int ret = routing_build_ack(MY_NODE, NODE_DST, MY_NODE, 0x0001u,
                                 0u, 0u, small, sizeof(small));
    CHECK(ret == -1, "routing_build_ack returns -1 on buffer-too-small");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * main
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    test_stubs_init();

    run1_enqueue_ack();
    run2_timeout_retry();
    run3_ack_after_retry();
    run4_exhausted_fallback();
    run5_table_full();
    run6_routing_build_ack();

    printf("\n──────────────────────────────────────────────────────────────────\n");
    printf("retry suite: %u PASS  %u FAIL\n", (unsigned)s_pass, (unsigned)s_fail);
    return (s_fail > 0u) ? 1 : 0;
}
