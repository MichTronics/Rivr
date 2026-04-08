/**
 * @file  test_private_chat.c
 * @brief Unit-tests for the private chat engine (encode/decode, send/receive,
 *        delivery states, dedup, expiry, queue limits, receipt correlation).
 *
 * RUN 1  — encode/decode private chat payload (round-trip, various body sizes)
 * RUN 2  — encode/decode delivery receipt (round-trip + _Static_assert size)
 * RUN 3  — dedup suppression (same msg_id + from_id → PCHAT_ERR_DUPLICATE)
 * RUN 4  — receipt correlation (outgoing entry → PCHAT_STATE_DELIVERED)
 * RUN 5  — queue full rejection (PCHAT_ERR_QUEUE_FULL after QUEUE_SIZE sends)
 * RUN 6  — invalid peer rejection (peer==0, peer==0xFFFFFFFF)
 * RUN 7  — body too long rejection (PCHAT_ERR_BODY_TOO_LONG)
 * RUN 8  — dst_mismatch rejection (frame addressed to a different node)
 * RUN 9  — malformed payload rejection (body_len overflows declared pay_len)
 * RUN 10 — expiry tick: QUEUED entry → PCHAT_STATE_FAILED_NO_ROUTE
 * RUN 11 — expiry tick: FORWARDED entry → PCHAT_STATE_DELIVERY_UNCONFIRMED
 * RUN 12 — wire-ACK success: PCHAT_STATE_SENT → PCHAT_STATE_FORWARDED
 * RUN 13 — wire-ACK failure (retry exhausted): → PCHAT_STATE_FAILED_RETRY
 * RUN 14 — receipt rate-limiting (> PRIVATE_CHAT_RECEIPT_RATE_MAX per window)
 *
 * Build: see tests/Makefile target 'private_chat'.
 * Exit:  0 = all checks passed, 1 = at least one failure.
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
#include "route_cache.h"
#include "pending_queue.h"
#include "retry_table.h"
#include "rivr_metrics.h"
#include "private_chat.h"
#include "timebase.h"

/* ── External stubs (test_stubs.c) ──────────────────────────────────────── */
extern void     test_stubs_init(void);
extern void     test_advance_ms(uint32_t delta_ms);
extern void     test_set_ms(uint32_t abs_ms);
extern atomic_uint_fast32_t g_mono_ms;
extern rb_t rf_tx_queue;

/* ── Module-global state referenced by private_chat.c ──────────────────── */
uint32_t         g_my_node_id   = 0u;
pending_queue_t  g_pending_queue;
retry_table_t    g_retry_table;
route_cache_t    g_route_cache;

/* BLE push stubs — private_chat.c calls these; keep them no-ops here */
void rivr_ble_companion_push_private_chat_rx(
        uint64_t msg_id, uint32_t from_id, uint32_t to_id,
        uint32_t sender_seq, uint32_t timestamp_s, uint16_t flags,
        const uint8_t *body, uint8_t body_len)
{
    (void)msg_id; (void)from_id; (void)to_id;
    (void)sender_seq; (void)timestamp_s; (void)flags;
    (void)body; (void)body_len;
}
void rivr_ble_companion_push_pchat_state(uint64_t msg_id,
                                          uint32_t peer_id, uint8_t state)
{
    (void)msg_id; (void)peer_id; (void)state;
}
void rivr_ble_companion_push_delivery_receipt(uint64_t orig_msg_id,
                                               uint32_t sender_id,
                                               uint32_t timestamp_s,
                                               uint8_t status)
{
    (void)orig_msg_id; (void)sender_id; (void)timestamp_s; (void)status;
}

/* routing_next_pkt_id stub (normally in routing.c) */
static uint32_t s_pkt_id_ctr = 0x1000u;
uint16_t routing_next_pkt_id(void) { return (uint16_t)(++s_pkt_id_ctr); }

/* ── Node IDs ─────────────────────────────────────────────────────────────── */
#define MY_NODE   0xFEED0001ul
#define PEER_NODE 0xBEEF0002ul
#define OTHER_NODE 0xAAAA0003ul

/* ── Assertion framework ─────────────────────────────────────────────────── */
static uint32_t s_pass = 0u;
static uint32_t s_fail = 0u;

#define CHECK(cond, msg) do {                                                  \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }                      \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg), __FILE__, __LINE__);     \
                 s_fail++; }                                                   \
} while (0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Drain all frames from rf_tx_queue; return count. */
static uint32_t drain_tx(void)
{
    uint32_t n = 0u;
    rf_tx_request_t dummy;
    while (rb_pop(&rf_tx_queue, &dummy)) { n++; }
    return n;
}

/** Find entry in g_pchat_queue by msg_id. */
static pchat_entry_t *find_entry(uint64_t msg_id)
{
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        if (g_pchat_queue[i].valid && g_pchat_queue[i].msg_id == msg_id) {
            return &g_pchat_queue[i];
        }
    }
    return NULL;
}

/** Build a minimal valid PRIVATE_CHAT reception header. */
static rivr_pkt_hdr_t make_rx_hdr(uint32_t src, uint32_t dst, uint16_t pkt_id)
{
    rivr_pkt_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic       = RIVR_MAGIC;
    h.version     = RIVR_PROTO_VER;
    h.pkt_type    = PKT_PRIVATE_CHAT;
    h.flags       = PKT_FLAG_ACK_REQ;
    h.ttl         = 7u;
    h.hop         = 0u;
    h.net_id      = 0u;
    h.src_id      = src;
    h.dst_id      = dst;
    h.seq         = 0u;
    h.pkt_id      = pkt_id;
    h.payload_len = 0u; /* updated by encode result */
    return h;
}

/** Build a minimal DELIVERY_RECEIPT reception header. */
static rivr_pkt_hdr_t make_rcpt_hdr(uint32_t src, uint32_t dst)
{
    rivr_pkt_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic    = RIVR_MAGIC;
    h.version  = RIVR_PROTO_VER;
    h.pkt_type = PKT_DELIVERY_RECEIPT;
    h.flags    = 0u;
    h.ttl      = 7u;
    h.src_id   = src;
    h.dst_id   = dst;
    return h;
}

/** Reset all engine state and per-test globals. */
static void reset(void)
{
    g_my_node_id = MY_NODE;
    memset(&g_pending_queue, 0, sizeof(g_pending_queue));
    retry_table_init(&g_retry_table);
    route_cache_init(&g_route_cache);
    private_chat_init();
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));
    drain_tx();
    s_pkt_id_ctr = 0x1000u;
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 1 — encode/decode private chat payload round-trip
 * ══════════════════════════════════════════════════════════════════════════ */
static void run1_payload_roundtrip(void)
{
    printf("\n── RUN 1: payload encode/decode round-trip ─────────────────────\n");

    /* 1a: empty body */
    {
        private_chat_payload_t in, out;
        memset(&in, 0, sizeof(in));
        in.msg_id          = 0xDEADBEEFFEED0001ULL;
        in.sender_seq      = 42u;
        in.timestamp_s     = 1700000000u;
        in.flags           = PCHAT_FLAGS_DEFAULT;
        in.expires_delta_s = 30u;
        in.reply_to_msg_id = 0u;
        in.body_len        = 0u;

        uint8_t buf[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
        int len = private_chat_encode_payload(&in, buf, sizeof(buf));
        CHECK(len == (int)PRIVATE_CHAT_PAYLOAD_HDR_LEN, "empty body encodes to HDR_LEN bytes");

        pchat_error_t rc = private_chat_decode_payload(buf, (uint8_t)len, &out);
        CHECK(rc == PCHAT_OK,                             "empty body decodes OK");
        CHECK(out.msg_id      == in.msg_id,               "msg_id round-trips");
        CHECK(out.sender_seq  == in.sender_seq,           "sender_seq round-trips");
        CHECK(out.timestamp_s == in.timestamp_s,          "timestamp_s round-trips");
        CHECK(out.flags       == in.flags,                "flags round-trips");
        CHECK(out.expires_delta_s == in.expires_delta_s,  "expires_delta_s round-trips");
        CHECK(out.body_len    == 0u,                      "body_len==0 round-trips");
    }

    /* 1b: body with text */
    {
        private_chat_payload_t in, out;
        memset(&in, 0, sizeof(in));
        in.msg_id          = 0x0000000100000002ULL;
        in.sender_seq      = 7u;
        in.timestamp_s     = 0u;
        in.flags           = PCHAT_FLAG_USER_VISIBLE;
        in.expires_delta_s = 0u;
        in.reply_to_msg_id = 0xABCDF00D12345678ULL;
        const char *txt    = "Hello, mesh!";
        in.body_len        = (uint8_t)strlen(txt);
        memcpy(in.body, txt, in.body_len);

        uint8_t buf[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
        int len = private_chat_encode_payload(&in, buf, sizeof(buf));
        CHECK(len == (int)(PRIVATE_CHAT_PAYLOAD_HDR_LEN + in.body_len),
              "body present: length = HDR + body_len");

        pchat_error_t rc = private_chat_decode_payload(buf, (uint8_t)len, &out);
        CHECK(rc == PCHAT_OK, "body decode returns PCHAT_OK");
        CHECK(out.body_len == in.body_len, "body_len preserved");
        CHECK(memcmp(out.body, in.body, in.body_len) == 0, "body content preserved");
        CHECK(out.reply_to_msg_id == in.reply_to_msg_id, "reply_to_msg_id preserved");
    }

    /* 1c: max body */
    {
        private_chat_payload_t in, out;
        memset(&in, 0, sizeof(in));
        in.msg_id    = 0x1111111122222222ULL;
        in.body_len  = PRIVATE_CHAT_MAX_BODY;
        memset(in.body, 0xAB, PRIVATE_CHAT_MAX_BODY);

        uint8_t buf[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
        int len = private_chat_encode_payload(&in, buf, sizeof(buf));
        CHECK(len == (int)PRIVATE_CHAT_MAX_PAYLOAD_LEN, "max body encodes to MAX_PAYLOAD_LEN");

        pchat_error_t rc = private_chat_decode_payload(buf, (uint8_t)len, &out);
        CHECK(rc == PCHAT_OK, "max body decodes OK");
        CHECK(out.body_len == PRIVATE_CHAT_MAX_BODY, "max body_len preserved");
        CHECK(memcmp(out.body, in.body, PRIVATE_CHAT_MAX_BODY) == 0, "max body content OK");
    }

    /* 1d: reject payload shorter than header */
    {
        uint8_t short_buf[10];
        memset(short_buf, 0, sizeof(short_buf));
        private_chat_payload_t out;
        pchat_error_t rc = private_chat_decode_payload(short_buf, sizeof(short_buf), &out);
        CHECK(rc == PCHAT_ERR_INVALID_PAYLOAD, "short payload → PCHAT_ERR_INVALID_PAYLOAD");
    }
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 2 — encode/decode delivery receipt round-trip
 * ══════════════════════════════════════════════════════════════════════════ */
static void run2_receipt_roundtrip(void)
{
    printf("\n── RUN 2: receipt encode/decode round-trip ─────────────────────\n");

    delivery_receipt_payload_t in, out;
    memset(&in, 0, sizeof(in));
    in.orig_msg_id = 0xFEEDFACE00112233ULL;
    in.sender_id   = MY_NODE;
    in.timestamp_s = 1700001234u;
    in.status      = RCPT_STATUS_DELIVERED;

    uint8_t buf[DELIVERY_RECEIPT_PAYLOAD_LEN + 4u];
    int len = private_chat_encode_receipt(&in, buf, sizeof(buf));
    CHECK(len == (int)DELIVERY_RECEIPT_PAYLOAD_LEN, "receipt encodes to DELIVERY_RECEIPT_PAYLOAD_LEN bytes");

    pchat_error_t rc = private_chat_decode_receipt(buf, (uint8_t)len, &out);
    CHECK(rc == PCHAT_OK,                            "receipt decode returns PCHAT_OK");
    CHECK(out.orig_msg_id == in.orig_msg_id,         "orig_msg_id round-trips");
    CHECK(out.sender_id   == in.sender_id,           "sender_id round-trips");
    CHECK(out.timestamp_s == in.timestamp_s,         "timestamp_s round-trips");
    CHECK(out.status      == in.status,              "status round-trips");

    /* Verify wrong-length input is rejected. */
    pchat_error_t rc2 = private_chat_decode_receipt(buf, DELIVERY_RECEIPT_PAYLOAD_LEN - 1u, &out);
    CHECK(rc2 == PCHAT_ERR_INVALID_PAYLOAD, "short receipt → PCHAT_ERR_INVALID_PAYLOAD");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 3 — dedup suppression
 * ══════════════════════════════════════════════════════════════════════════ */
static void run3_dedup(void)
{
    printf("\n── RUN 3: dedup suppression ──────────────────────────────────────\n");
    reset();

    /* Build a valid payload. */
    private_chat_payload_t p;
    memset(&p, 0, sizeof(p));
    p.msg_id          = 0x0000000100000099ULL;
    p.sender_seq      = 99u;
    p.timestamp_s     = 0u;
    p.flags           = PCHAT_FLAGS_DEFAULT & ~((uint16_t)PCHAT_FLAG_RECEIPT_REQ);
    p.expires_delta_s = 0u;
    p.body_len        = 4u;
    memcpy(p.body, "test", 4u);

    uint8_t pay_buf[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
    int pay_len = private_chat_encode_payload(&p, pay_buf, sizeof(pay_buf));

    rivr_pkt_hdr_t hdr = make_rx_hdr(PEER_NODE, MY_NODE, 0x0001u);
    hdr.payload_len = (uint8_t)pay_len;

    /* First reception should succeed. */
    pchat_error_t rc1 = private_chat_on_rx(&hdr, pay_buf, (uint8_t)pay_len);
    CHECK(rc1 == PCHAT_OK, "first rx returns PCHAT_OK");
    CHECK(g_rivr_metrics.private_chat_rx_total == 1u, "private_chat_rx_total == 1");

    /* Second reception with same msg_id + from_id must be deduplicated. */
    pchat_error_t rc2 = private_chat_on_rx(&hdr, pay_buf, (uint8_t)pay_len);
    CHECK(rc2 == PCHAT_ERR_DUPLICATE, "duplicate rx returns PCHAT_ERR_DUPLICATE");
    CHECK(g_rivr_metrics.private_chat_dedup_drop_total == 1u, "dedup_drop_total == 1");
    CHECK(g_rivr_metrics.private_chat_rx_total == 1u, "rx_total unchanged after dedup");

    /* Different from_id with same msg_id passes through (different sender). */
    rivr_pkt_hdr_t hdr2 = make_rx_hdr(OTHER_NODE, MY_NODE, 0x0002u);
    hdr2.payload_len = (uint8_t)pay_len;
    pchat_error_t rc3 = private_chat_on_rx(&hdr2, pay_buf, (uint8_t)pay_len);
    CHECK(rc3 == PCHAT_OK, "same msg_id from different sender passes dedup");
    CHECK(g_rivr_metrics.private_chat_rx_total == 2u, "rx_total == 2 after two unique senders");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 4 — receipt correlation → PCHAT_STATE_DELIVERED
 * ══════════════════════════════════════════════════════════════════════════ */
static void run4_receipt_correlation(void)
{
    printf("\n── RUN 4: receipt correlation → DELIVERED ──────────────────────\n");
    reset();

    /* Route cache is empty → send will go to AWAITING_ROUTE. */
    uint64_t msg_id = 0u;
    pchat_error_t rc = private_chat_send(PEER_NODE,
                                          (const uint8_t *)"hi", 2u,
                                          &msg_id);
    CHECK(rc == PCHAT_OK,    "send returns PCHAT_OK");
    CHECK(msg_id != 0u,      "msg_id is non-zero");

    pchat_entry_t *e = find_entry(msg_id);
    CHECK(e != NULL,         "entry found in queue");
    if (!e) return;
    CHECK(e->state == PCHAT_STATE_AWAITING_ROUTE, "state == AWAITING_ROUTE (no route)");

    /* Manually force state to FORWARDED to simulate a sent+acked message. */
    e->state   = PCHAT_STATE_FORWARDED;
    e->sent_ms = tb_millis();
    uint16_t saved_pkt_id = e->pkt_id;

    /* Simulate arrival of a delivery receipt. */
    delivery_receipt_payload_t r;
    r.orig_msg_id = msg_id;
    r.sender_id   = MY_NODE;  /* receipt says: original sender was us */
    r.timestamp_s = 0u;
    r.status      = RCPT_STATUS_DELIVERED;

    uint8_t rcpt_buf[DELIVERY_RECEIPT_PAYLOAD_LEN];
    int rcpt_len = private_chat_encode_receipt(&r, rcpt_buf, sizeof(rcpt_buf));
    CHECK(rcpt_len == (int)DELIVERY_RECEIPT_PAYLOAD_LEN, "receipt encodes to correct length");

    rivr_pkt_hdr_t rcpt_hdr = make_rcpt_hdr(PEER_NODE, MY_NODE);

    pchat_error_t rrc = private_chat_on_receipt(&rcpt_hdr, rcpt_buf, (uint8_t)rcpt_len);
    CHECK(rrc == PCHAT_OK, "on_receipt returns PCHAT_OK");
    CHECK(g_rivr_metrics.private_chat_receipt_rx_total == 1u, "receipt_rx_total == 1");

    /* Entry should now be released (valid=false after DELIVERED). */
    /* The delivery state was notified and entry released. */
    CHECK(!e->valid, "entry released after DELIVERED receipt");
    (void)saved_pkt_id;
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 5 — queue full rejection
 * ══════════════════════════════════════════════════════════════════════════ */
static void run5_queue_full(void)
{
    printf("\n── RUN 5: queue full rejection ───────────────────────────────────\n");
    reset();

    uint64_t ids[PRIVATE_CHAT_QUEUE_SIZE];
    const uint8_t body[] = "x";

    /* Fill the queue. */
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        pchat_error_t rc = private_chat_send(PEER_NODE, body, 1u, &ids[i]);
        CHECK(rc == PCHAT_OK, "send into empty slot succeeds");
    }

    /* One more send must fail with PCHAT_ERR_QUEUE_FULL. */
    uint64_t overflow_id = 0u;
    pchat_error_t rc_full = private_chat_send(PEER_NODE, body, 1u, &overflow_id);
    CHECK(rc_full == PCHAT_ERR_QUEUE_FULL, "send to full queue → PCHAT_ERR_QUEUE_FULL");
    CHECK(private_chat_queue_depth() == PRIVATE_CHAT_QUEUE_SIZE,
          "queue_depth == PRIVATE_CHAT_QUEUE_SIZE when full");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 6 — invalid peer rejection
 * ══════════════════════════════════════════════════════════════════════════ */
static void run6_invalid_peer(void)
{
    printf("\n── RUN 6: invalid peer rejection ─────────────────────────────────\n");
    reset();

    const uint8_t body[] = "test";
    uint64_t id = 0u;

    pchat_error_t rc0 = private_chat_send(0u, body, sizeof(body), &id);
    CHECK(rc0 == PCHAT_ERR_INVALID_PEER, "peer==0 → PCHAT_ERR_INVALID_PEER");

    pchat_error_t rcff = private_chat_send(0xFFFFFFFFu, body, sizeof(body), &id);
    CHECK(rcff == PCHAT_ERR_INVALID_PEER, "peer==0xFFFFFFFF → PCHAT_ERR_INVALID_PEER");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 7 — body too long rejection
 * ══════════════════════════════════════════════════════════════════════════ */
static void run7_body_too_long(void)
{
    printf("\n── RUN 7: body too long rejection ────────────────────────────────\n");
    reset();

    uint8_t big_body[PRIVATE_CHAT_MAX_BODY + 1u];
    memset(big_body, 'A', sizeof(big_body));

    uint64_t id = 0u;
    pchat_error_t rc = private_chat_send(PEER_NODE, big_body,
                                          PRIVATE_CHAT_MAX_BODY + 1u, &id);
    CHECK(rc == PCHAT_ERR_BODY_TOO_LONG, "oversized body → PCHAT_ERR_BODY_TOO_LONG");
    CHECK(g_rivr_metrics.private_chat_invalid_total >= 1u,
          "private_chat_invalid_total incremented");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 8 — dst_mismatch rejection
 * ══════════════════════════════════════════════════════════════════════════ */
static void run8_dst_mismatch(void)
{
    printf("\n── RUN 8: dst_mismatch rejection ─────────────────────────────────\n");
    reset();

    private_chat_payload_t p;
    memset(&p, 0, sizeof(p));
    p.msg_id     = 0xAABBCCDD00000001ULL;
    p.sender_seq = 1u;
    p.flags      = PCHAT_FLAGS_DEFAULT;
    p.body_len   = 3u;
    memcpy(p.body, "abc", 3u);

    uint8_t pay[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
    int plen = private_chat_encode_payload(&p, pay, sizeof(pay));

    /* Frame addressed to OTHER_NODE, not MY_NODE → must be rejected. */
    rivr_pkt_hdr_t hdr = make_rx_hdr(PEER_NODE, OTHER_NODE, 0xDEADu);
    hdr.payload_len = (uint8_t)plen;

    pchat_error_t rc = private_chat_on_rx(&hdr, pay, (uint8_t)plen);
    CHECK(rc == PCHAT_ERR_DST_MISMATCH, "dst!=my_node → PCHAT_ERR_DST_MISMATCH");
    CHECK(g_rivr_metrics.private_chat_invalid_total >= 1u, "invalid_total incremented");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 9 — malformed payload rejection (body_len overflows declared pay_len)
 * ══════════════════════════════════════════════════════════════════════════ */
static void run9_malformed_payload(void)
{
    printf("\n── RUN 9: malformed payload rejection ───────────────────────────\n");
    reset();

    /* Craft a header-only payload but set body_len to non-zero. */
    uint8_t bad_pay[PRIVATE_CHAT_PAYLOAD_HDR_LEN];
    memset(bad_pay, 0, sizeof(bad_pay));
    bad_pay[28] = 50u; /* body_len claims 50 bytes but pay_len == HDR_LEN only */

    rivr_pkt_hdr_t hdr = make_rx_hdr(PEER_NODE, MY_NODE, 0x0007u);
    hdr.payload_len = (uint8_t)sizeof(bad_pay);

    uint32_t inv_before = g_rivr_metrics.private_chat_invalid_total;
    pchat_error_t rc = private_chat_on_rx(&hdr, bad_pay, (uint8_t)sizeof(bad_pay));
    CHECK(rc == PCHAT_ERR_INVALID_PAYLOAD, "body_len overflow → PCHAT_ERR_INVALID_PAYLOAD");
    CHECK(g_rivr_metrics.private_chat_invalid_total > inv_before,
          "invalid_total incremented for malformed payload");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 10 — expiry tick: QUEUED entry → FAILED_NO_ROUTE
 * ══════════════════════════════════════════════════════════════════════════ */
static void run10_expiry_no_route(void)
{
    printf("\n── RUN 10: expiry tick – QUEUED → FAILED_NO_ROUTE ────────────────\n");
    reset();

    /* No route in cache → send goes to AWAITING_ROUTE. */
    uint64_t msg_id = 0u;
    pchat_error_t rc = private_chat_send(PEER_NODE,
                                          (const uint8_t *)"expire me", 9u,
                                          &msg_id);
    CHECK(rc == PCHAT_OK, "send returns OK");

    pchat_entry_t *e = find_entry(msg_id);
    CHECK(e != NULL, "entry found");
    if (!e) return;
    CHECK(e->state == PCHAT_STATE_AWAITING_ROUTE, "state == AWAITING_ROUTE");

    /* Advance clock past the queue expiry. */
    test_advance_ms(PRIVATE_CHAT_QUEUE_EXPIRY_MS + 100u);
    uint32_t now = tb_millis();
    private_chat_tick(now);

    /* Entry should be released with FAILED_NO_ROUTE. */
    CHECK(!e->valid, "entry released after expiry");
    CHECK(g_rivr_metrics.private_chat_failed_no_route_total >= 1u,
          "failed_no_route_total incremented");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 11 — expiry tick: FORWARDED entry → DELIVERY_UNCONFIRMED
 * ══════════════════════════════════════════════════════════════════════════ */
static void run11_expiry_forwarded(void)
{
    printf("\n── RUN 11: expiry tick – FORWARDED → DELIVERY_UNCONFIRMED ────────\n");
    reset();

    uint64_t msg_id = 0u;
    pchat_error_t rc = private_chat_send(PEER_NODE,
                                          (const uint8_t *)"forward test", 12u,
                                          &msg_id);
    CHECK(rc == PCHAT_OK, "send returns OK");

    pchat_entry_t *e = find_entry(msg_id);
    CHECK(e != NULL, "entry found");
    if (!e) return;

    /* Force to FORWARDED state with sent_ms set. */
    e->state    = PCHAT_STATE_FORWARDED;
    e->sent_ms  = tb_millis();

    /* Advance past receipt timeout. */
    test_advance_ms(PRIVATE_CHAT_RECEIPT_TMO_MS + 100u);
    uint32_t now = tb_millis();
    private_chat_tick(now);

    CHECK(!e->valid, "entry released after receipt timeout");
    CHECK(g_rivr_metrics.private_chat_receipt_timeout_total >= 1u,
          "receipt_timeout_total incremented");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 12 — wire-ACK success: SENT → FORWARDED
 * ══════════════════════════════════════════════════════════════════════════ */
static void run12_wire_ack_success(void)
{
    printf("\n── RUN 12: wire-ACK success – SENT → FORWARDED ──────────────────\n");
    reset();

    uint64_t msg_id = 0u;
    pchat_error_t rc = private_chat_send(PEER_NODE,
                                          (const uint8_t *)"ack me", 6u,
                                          &msg_id);
    CHECK(rc == PCHAT_OK, "send returns OK");

    pchat_entry_t *e = find_entry(msg_id);
    if (!e) { printf("FAIL  entry not found\n"); s_fail++; return; }

    /* Force to SENT state. */
    e->state   = PCHAT_STATE_SENT;
    e->sent_ms = tb_millis();
    uint16_t pkt_id = e->pkt_id;

    /* Deliver wire ACK. */
    private_chat_on_wire_ack(pkt_id, MY_NODE, true);
    CHECK(e->state == PCHAT_STATE_FORWARDED, "ACK success → PCHAT_STATE_FORWARDED");
    CHECK(e->valid == true, "entry still valid after ACK (awaiting receipt)");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 13 — wire-ACK failure (retry exhausted): → FAILED_RETRY
 * ══════════════════════════════════════════════════════════════════════════ */
static void run13_wire_ack_failure(void)
{
    printf("\n── RUN 13: wire-ACK failure – → FAILED_RETRY ────────────────────\n");
    reset();

    uint64_t msg_id = 0u;
    private_chat_send(PEER_NODE, (const uint8_t *)"fail me", 7u, &msg_id);

    pchat_entry_t *e = find_entry(msg_id);
    if (!e) { printf("FAIL  entry not found\n"); s_fail++; return; }

    /* Force to SENT. */
    e->state   = PCHAT_STATE_SENT;
    e->sent_ms = tb_millis();
    uint16_t pkt_id = e->pkt_id;

    uint32_t fail_before = g_rivr_metrics.private_chat_failed_retry_budget_total;

    /* Retry exhaustion delivers success=false. */
    private_chat_on_wire_ack(pkt_id, MY_NODE, false);
    CHECK(!e->valid, "entry released after retry failure");
    CHECK(g_rivr_metrics.private_chat_failed_retry_budget_total > fail_before,
          "failed_retry_budget_total incremented");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 14 — receipt rate-limiting
 * ══════════════════════════════════════════════════════════════════════════ */
static void run14_receipt_rate_limit(void)
{
    printf("\n── RUN 14: receipt rate-limiting ────────────────────────────────\n");
    reset();

    /* Build a payload that requests a receipt. */
    private_chat_payload_t p;
    memset(&p, 0, sizeof(p));
    p.flags    = PCHAT_FLAGS_DEFAULT; /* includes PCHAT_FLAG_RECEIPT_REQ */
    p.body_len = 2u;
    memcpy(p.body, "hi", 2u);

    uint8_t pay[PRIVATE_CHAT_MAX_PAYLOAD_LEN];

    uint32_t rcpt_tx_before = g_rivr_metrics.private_chat_receipt_tx_total;

    /* Send PRIVATE_CHAT_RECEIPT_RATE_MAX messages from different senders
     * to exhaust the rate window. */
    for (uint8_t i = 0u; i < PRIVATE_CHAT_RECEIPT_RATE_MAX; i++) {
        p.msg_id     = 0x1000000000000000ULL | i;
        p.sender_seq = i;
        int plen = private_chat_encode_payload(&p, pay, sizeof(pay));
        rivr_pkt_hdr_t hdr = make_rx_hdr((uint32_t)(0xBB000000u | i), MY_NODE, (uint16_t)i);
        hdr.payload_len = (uint8_t)plen;
        private_chat_on_rx(&hdr, pay, (uint8_t)plen);
    }

    uint32_t rcpt_tx_after = g_rivr_metrics.private_chat_receipt_tx_total;
    CHECK(rcpt_tx_after - rcpt_tx_before == PRIVATE_CHAT_RECEIPT_RATE_MAX,
          "receipt_tx_total incremented for each message up to rate limit");

    /* One more message in the same time window must NOT emit a receipt. */
    p.msg_id     = 0x2000000000000000ULL;
    p.sender_seq = 100u;
    int plen2 = private_chat_encode_payload(&p, pay, sizeof(pay));
    rivr_pkt_hdr_t hdr2 = make_rx_hdr(0xCC000001u, MY_NODE, 0x0999u);
    hdr2.payload_len = (uint8_t)plen2;
    private_chat_on_rx(&hdr2, pay, (uint8_t)plen2);

    CHECK(g_rivr_metrics.private_chat_receipt_tx_total == rcpt_tx_after,
          "rate-limited: receipt_tx_total not incremented beyond window max");

    /* Advance past the rate window; next message should emit a receipt again. */
    test_advance_ms(PRIVATE_CHAT_RECEIPT_RATE_WIN_MS + 1000u);
    p.msg_id     = 0x3000000000000000ULL;
    p.sender_seq = 200u;
    int plen3 = private_chat_encode_payload(&p, pay, sizeof(pay));
    rivr_pkt_hdr_t hdr3 = make_rx_hdr(0xDD000001u, MY_NODE, 0x1001u);
    hdr3.payload_len = (uint8_t)plen3;
    private_chat_on_rx(&hdr3, pay, (uint8_t)plen3);

    CHECK(g_rivr_metrics.private_chat_receipt_tx_total == rcpt_tx_after + 1u,
          "after rate window expiry, receipt emitted again");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    test_stubs_init();

    run1_payload_roundtrip();
    run2_receipt_roundtrip();
    run3_dedup();
    run4_receipt_correlation();
    run5_queue_full();
    run6_invalid_peer();
    run7_body_too_long();
    run8_dst_mismatch();
    run9_malformed_payload();
    run10_expiry_no_route();
    run11_expiry_forwarded();
    run12_wire_ack_success();
    run13_wire_ack_failure();
    run14_receipt_rate_limit();

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  Private chat tests: %u passed, %u failed\n", s_pass, s_fail);
    printf("══════════════════════════════════════════════════════════════════\n\n");

    return (s_fail == 0u) ? 0 : 1;
}
