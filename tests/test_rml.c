#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firmware_core/rml/rml.h"

static uint32_t s_pass;
static uint32_t s_fail;

#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }          \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg),              \
                        __FILE__, __LINE__); s_fail++; }           \
} while (0)

static rml_message_t make_sample(void)
{
    static const uint8_t payload[] = { 'h', 'e', 'l', 'l', 'o' };
    rml_message_t msg;
    bool ok = rml_make_chat(0x1234u, 0u, 0xABCDEF01u,
                            payload, (uint16_t)sizeof(payload), &msg);
    CHECK(ok, "sample chat creation");
    return msg;
}

static rml_context_t make_context(void)
{
    rml_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.local_id = 0x8888u;
    ctx.now_s = 100u;
    ctx.now_ms = 100000u;
    ctx.has_time = true;
    ctx.hard_radio_ok = true;
    ctx.duty_budget_us = 1000000u;
    ctx.duty_remaining_us = 1000000u;
    ctx.rssi_dbm = -88;
    ctx.snr_db = 7;
    ctx.local_role = RML_ROLE_REPEATER;
    ctx.queue_capacity = 4u;
    ctx.queue_depth = 0u;
    return ctx;
}

static rml_message_t make_policy_msg(uint32_t msg_id, uint8_t intent, uint8_t ttl)
{
    static const uint8_t payload[] = { 'p' };
    rml_message_t msg;
    bool ok = rml_make_chat(0x1234u, 0u, 0x10u, payload, (uint16_t)sizeof(payload), &msg);
    CHECK(ok, "policy message creation");
    msg.msg_id = msg_id;
    msg.intent = intent;
    msg.ttl = ttl;
    msg.priority = 3u;
    return msg;
}

static rml_message_t make_chat_msg(uint32_t msg_id,
                                   uint32_t thread_id,
                                   const char *text)
{
    rml_message_t msg;
    bool ok = rml_make_chat(0x1234u, 0u, thread_id,
                            (const uint8_t *)text,
                            (uint16_t)strlen(text), &msg);
    CHECK(ok, "thread chat creation");
    msg.msg_id = msg_id;
    return msg;
}

static rml_delta_t make_delta(uint8_t op,
                              uint16_t offset,
                              uint16_t delete_len,
                              const char *text)
{
    rml_delta_t delta;
    memset(&delta, 0, sizeof(delta));
    delta.op = op;
    delta.offset = offset;
    delta.delete_len = delete_len;
    if (text) {
        delta.data_len = (uint16_t)strlen(text);
        memcpy(delta.data, text, delta.data_len);
    }
    return delta;
}

static void test_roundtrip(void)
{
    printf("\n=== RML roundtrip ===\n");
    rml_init();
    rml_message_t msg = make_sample();
    uint8_t wire[256];
    size_t wire_len = 0u;
    CHECK(rml_encode(&msg, wire, sizeof(wire), &wire_len), "encode succeeds");
    CHECK(wire_len == RML_WIRE_HDR_LEN + msg.payload_len, "encoded size");
    rml_message_t out;
    CHECK(rml_decode(wire, wire_len, &out), "decode succeeds");
    CHECK(out.magic == RML_MAGIC && out.version == RML_VERSION, "magic/version roundtrip");
    CHECK(out.msg_id == msg.msg_id, "msg_id roundtrip");
    CHECK(out.payload_len == msg.payload_len, "payload_len roundtrip");
    CHECK(memcmp(out.payload, msg.payload, msg.payload_len) == 0, "payload roundtrip");
}

static void test_bad_magic_version(void)
{
    printf("\n=== RML bad magic/version ===\n");
    rml_init();
    rml_message_t msg = make_sample();
    uint8_t wire[256];
    size_t wire_len = 0u;
    rml_encode(&msg, wire, sizeof(wire), &wire_len);
    rml_message_t out;
    wire[0] = 0x00u;
    CHECK(!rml_decode(wire, wire_len, &out), "bad magic rejected");
    CHECK(g_rml_stats.rml_rx_bad_magic == 1u, "bad magic counter");
    wire[0] = RML_MAGIC;
    wire[1] = 0x02u;
    CHECK(!rml_decode(wire, wire_len, &out), "bad version rejected");
    CHECK(g_rml_stats.rml_rx_bad_version == 1u, "bad version counter");
}

static void test_payload_bounds(void)
{
    printf("\n=== RML payload bounds ===\n");
    rml_init();
    uint8_t payload[RML_MAX_PAYLOAD + 1u];
    memset(payload, 0xA5, sizeof(payload));
    rml_message_t msg;
    CHECK(!rml_make_chat(1u, 0u, 0u, payload, (uint16_t)sizeof(payload), &msg),
          "make_chat rejects oversize payload");

    CHECK(rml_make_chat(1u, 0u, 0u, payload, RML_MAX_PAYLOAD, &msg),
          "make_chat accepts max payload");
    uint8_t wire[RML_WIRE_HDR_LEN + RML_MAX_PAYLOAD];
    size_t wire_len = 0u;
    CHECK(rml_encode(&msg, wire, sizeof(wire), &wire_len), "encode max payload");

    wire[26] = (uint8_t)((RML_MAX_PAYLOAD + 1u) & 0xFFu);
    wire[27] = (uint8_t)((RML_MAX_PAYLOAD + 1u) >> 8);
    rml_message_t out;
    CHECK(!rml_decode(wire, sizeof(wire), &out), "decode rejects oversize payload_len");
    CHECK(g_rml_stats.rml_rx_too_large == 1u, "too_large counter");
}

static void test_seen_and_ttl(void)
{
    printf("\n=== RML seen + ttl ===\n");
    rml_init();
    rml_message_t msg = make_sample();
    rml_context_t ctx = make_context();
    rml_policy_decision_t d;
    CHECK(!rml_seen(msg.msg_id), "initially unseen");
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.reason == RML_REASON_OK, "first accept succeeds");
    CHECK(rml_seen(msg.msg_id), "accept marks seen");
    d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.reason == RML_REASON_SEEN, "duplicate accept rejected");
    CHECK(g_rml_stats.rml_rx_seen_drop == 1u, "seen_drop counter");

    rml_init();
    msg = make_sample();
    msg.ttl = 0u;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && !d.relay && d.reason == RML_REASON_TTL_ZERO, "ttl zero relay drop");
    CHECK(g_rml_stats.rml_relay_ttl_drop == 1u, "ttl_drop counter");
}

static void test_policy_reasons(void)
{
    printf("\n=== RML policy reasons ===\n");
    rml_context_t ctx = make_context();
    rml_policy_decision_t d;
    rml_message_t msg;

    rml_init();
    msg = make_policy_msg(0x2001u, RML_INTENT_CHAT, 4u);
    msg.expires_s = 99u;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.reason == RML_REASON_EXPIRED, "expired rejected");

    rml_init();
    msg = make_policy_msg(0x2002u, RML_INTENT_CHAT, 4u);
    ctx.duty_cycle_blocked = true;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && !d.relay && d.reason == RML_REASON_DUTY_BLOCKED, "duty blocked suppresses relay");

    rml_init();
    ctx = make_context();
    msg = make_policy_msg(0x2003u, RML_INTENT_TELEMETRY, 4u);
    ctx.queue_pressure = 95u;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.reason == RML_REASON_QUEUE_FULL, "telemetry drops under pressure");

    rml_init();
    ctx = make_context();
    msg = make_policy_msg(0x3000u, RML_INTENT_STATUS, 3u);
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.reason == RML_REASON_OK, "new status accepted");
    msg.msg_id = 0x2FFFu;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.reason == RML_REASON_REPLACED_BY_NEWER_STATUS,
          "older status replaced by newer");

        rml_init();
        ctx = make_context();
        msg = make_policy_msg(0x3100u, RML_INTENT_STATUS, 3u);
        msg.state_type = 7u;
        d = rml_policy_decide(&msg, &ctx);
        CHECK(d.accept, "status state_type 7 accepted");
        msg.msg_id = 0x30FFu;
        msg.state_type = 8u;
        d = rml_policy_decide(&msg, &ctx);
        CHECK(d.accept, "older status for different state_type accepted");

        rml_init();
        ctx = make_context();
        msg = make_policy_msg(0x3200u, RML_INTENT_CHAT, 4u);
        ctx.duty_remaining_us = 10000u;
        d = rml_policy_decide(&msg, &ctx);
        CHECK(d.accept && !d.relay && d.reason == RML_REASON_DUTY_BLOCKED,
                    "intent airtime budget suppresses relay");
}

static void test_intent_policy(void)
{
    printf("\n=== RML intent policy ===\n");
    rml_context_t ctx = make_context();
    rml_policy_decision_t d;
    rml_message_t msg;

    rml_init();
    msg = make_policy_msg(0x4001u, RML_INTENT_CHAT, 5u);
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.relay && d.reason == RML_REASON_OK, "chat relays");
    CHECK(d.delay_ms >= 90u && d.delay_ms <= 420u, "chat deterministic delay range");
    uint16_t first_delay = d.delay_ms;
    rml_init();
    msg = make_policy_msg(0x4001u, RML_INTENT_CHAT, 5u);
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.delay_ms == first_delay, "chat delay deterministic for same inputs");

    rml_init();
    msg = make_policy_msg(0x4002u, RML_INTENT_PRIVATE, 5u);
    msg.target_id = 0x7777u;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && !d.relay && d.reason == RML_REASON_OK,
          "private without useful hint does not relay");
    rml_init();
    ctx.route_hint_valid = true;
    ctx.target_hint_useful = true;
    msg = make_policy_msg(0x4003u, RML_INTENT_PRIVATE, 5u);
    msg.target_id = 0x7777u;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.relay, "private relays with useful target hint");

    rml_init();
    ctx = make_context();
    msg = make_policy_msg(0x4004u, RML_INTENT_GROUP, 9u);
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.relay && d.next_ttl == 4u, "group clamps relay ttl strictly");

    rml_init();
    msg = make_policy_msg(0x4005u, RML_INTENT_EMERGENCY, 6u);
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.relay && d.delay_ms <= 8u, "emergency fast path delay");
    rml_init();
    ctx.duty_cycle_blocked = true;
    msg = make_policy_msg(0x4006u, RML_INTENT_EMERGENCY, 6u);
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && !d.relay && d.reason == RML_REASON_DUTY_BLOCKED,
          "emergency obeys hard safety");

    rml_init();
    ctx = make_context();
    ctx.queue_pressure = 95u;
    msg = make_policy_msg(0x4007u, RML_INTENT_TELEMETRY, 5u);
    msg.priority = 8u;
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.relay && d.reason == RML_REASON_OK,
          "high-priority telemetry survives pressure");
}

static void test_delta_append(void)
{
    printf("\n=== RML delta append ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_message_t base = make_chat_msg(0x5001u, 0x9001u, "hello");
    rml_policy_decision_t d = rml_policy_decide(&base, &ctx);
    CHECK(d.accept, "base chat accepted");

    rml_delta_t delta = make_delta(RML_DELTA_APPEND, 0u, 0u, " world");
    rml_message_t msg;
    CHECK(rml_make_delta(0x1234u, 0u, 0x9001u, 0x5002u, 0x5001u, &delta, &msg),
          "append delta creation");
    d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.reason == RML_REASON_DELTA_APPLIED, "append delta applied");
    CHECK(d.forward_reconstructed, "append delta requests reconstructed forwarding");

    rml_thread_entry_t entry;
    CHECK(rml_thread_cache_get(0x1234u, 0x9001u, &entry), "thread cache lookup");
    CHECK(entry.msg_id == 0x5002u, "append latest msg_id");
    CHECK(entry.payload_len == 11u && memcmp(entry.payload, "hello world", 11u) == 0,
          "append reconstruction text");
}

static void test_delta_replace(void)
{
    printf("\n=== RML delta replace ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_message_t base = make_chat_msg(0x5101u, 0x9002u, "hello world");
    rml_policy_decide(&base, &ctx);

    rml_delta_t delta = make_delta(RML_DELTA_REPLACE, 6u, 5u, "rivr");
    rml_message_t msg;
    CHECK(rml_make_delta(0x1234u, 0u, 0x9002u, 0x5102u, 0x5101u, &delta, &msg),
          "replace delta creation");
    rml_policy_decision_t d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.reason == RML_REASON_DELTA_APPLIED, "replace delta applied");

    rml_thread_entry_t entry;
    CHECK(rml_thread_cache_get(0x1234u, 0x9002u, &entry), "replace cache lookup");
    CHECK(entry.payload_len == 10u && memcmp(entry.payload, "hello rivr", 10u) == 0,
          "replace reconstruction text");
}

static void test_delta_delete_range(void)
{
    printf("\n=== RML delta delete range ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_message_t base = make_chat_msg(0x5201u, 0x9003u, "abcdef");
    rml_policy_decide(&base, &ctx);

    rml_delta_t delta = make_delta(RML_DELTA_DELETE_RANGE, 2u, 2u, NULL);
    rml_message_t msg;
    CHECK(rml_make_delta(0x1234u, 0u, 0x9003u, 0x5202u, 0x5201u, &delta, &msg),
          "delete delta creation");
    rml_policy_decision_t d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && d.reason == RML_REASON_DELTA_APPLIED, "delete delta applied");

    rml_thread_entry_t entry;
    CHECK(rml_thread_cache_get(0x1234u, 0x9003u, &entry), "delete cache lookup");
    CHECK(entry.payload_len == 4u && memcmp(entry.payload, "abef", 4u) == 0,
          "delete reconstruction text");
}

static void test_delta_missing_prev_and_repair(void)
{
    printf("\n=== RML missing prev + repair ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_delta_t delta = make_delta(RML_DELTA_APPEND, 0u, 0u, "x");
    rml_message_t msg;
    CHECK(rml_make_delta(0x1234u, 0u, 0x9004u, 0x5302u, 0x5301u, &delta, &msg),
          "missing-prev delta creation");
    rml_policy_decision_t d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.repair_needed && d.reason == RML_REASON_REPAIR_NEEDED,
          "missing prev creates repair-needed decision");
    CHECK(d.repair.missing_msg_id == 0x5301u && d.repair.thread_id == 0x9004u &&
          d.repair.sender_id == 0x1234u, "repair decision fields");
    CHECK(!rml_seen(0x5302u), "missing-prev delta not marked seen");

    rml_repair_request_t repair = d.repair;
    rml_message_t repair_msg;
    CHECK(rml_make_repair_request(0x8888u, 0x1234u, &repair, &repair_msg),
          "repair request creation");
    uint8_t wire[128];
    size_t wire_len = 0u;
    CHECK(rml_encode(&repair_msg, wire, sizeof(wire), &wire_len), "repair request encode");
    rml_message_t decoded;
    CHECK(rml_decode(wire, wire_len, &decoded), "repair request decode frame");
    rml_repair_request_t parsed;
    CHECK(rml_repair_request_decode(&decoded, &parsed), "repair request payload decode");
    CHECK(parsed.missing_msg_id == repair.missing_msg_id && parsed.thread_id == repair.thread_id &&
          parsed.sender_id == repair.sender_id, "repair request roundtrip fields");
    CHECK(d.repair_delay_ms > 0u, "repair request is delayed");

    rml_init();
    CHECK(rml_make_delta(0x1234u, 0u, 0x9004u, 0x5303u, 0u, &delta, &msg),
          "zero-prev delta creation");
    d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.repair_needed && d.reason == RML_REASON_MISSING_PREV,
          "zero prev_id reports missing-prev reason");
}

static void test_superseding_behavior(void)
{
    printf("\n=== RML superseding behavior ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_message_t base = make_chat_msg(0x8001u, 0xC001u, "old");
    rml_policy_decide(&base, &ctx);

    rml_message_t newer = make_chat_msg(0x8002u, 0xC001u, "new");
    newer.supersedes = true;
    newer.flags |= RML_FLAG_SUPERSEDES;
    rml_policy_decision_t d = rml_policy_decide(&newer, &ctx);
    CHECK(d.accept, "superseding message accepted");

    rml_thread_entry_t entry;
    CHECK(rml_thread_cache_get(0x1234u, 0xC001u, &entry), "superseded thread lookup");
    CHECK(entry.msg_id == 0x8002u && entry.payload_len == 3u && memcmp(entry.payload, "new", 3u) == 0,
          "superseding message becomes latest state");

    rml_delta_t delta = make_delta(RML_DELTA_APPEND, 0u, 0u, "x");
    rml_message_t stale_delta;
    CHECK(rml_make_delta(0x1234u, 0u, 0xC001u, 0x8003u, 0x8001u, &delta, &stale_delta),
          "stale-prev delta creation");
    d = rml_policy_decide(&stale_delta, &ctx);
    CHECK(!d.accept && d.repair_needed, "delta referencing superseded message needs repair");
}

static void test_relevance_decay(void)
{
    printf("\n=== RML relevance decay ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_message_t msg = make_policy_msg(0x8101u, RML_INTENT_CHAT, 5u);
    msg.priority = 2u;
    msg.created_ms = 1u;
    ctx.now_ms = 5u * 60000u;
    rml_policy_decision_t d = rml_policy_decide(&msg, &ctx);
    CHECK(d.accept && !d.relay && d.reason == RML_REASON_OK,
          "aged low-priority chat is accepted but not relayed");
}

static void test_soft_state_expiry(void)
{
    printf("\n=== RML soft state expiry ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    rml_message_t msg = make_chat_msg(0x8201u, 0xC101u, "temp");
    msg.soft_ttl_s = 1u;
    rml_policy_decide(&msg, &ctx);

    rml_thread_entry_t entry;
    CHECK(rml_thread_cache_get(0x1234u, 0xC101u, &entry), "soft-state entry initially present");

    ctx.now_ms += 1001u;
    rml_message_t tick = make_chat_msg(0x8202u, 0xC102u, "tick");
    rml_policy_decide(&tick, &ctx);
    CHECK(!rml_thread_cache_get(0x1234u, 0xC101u, &entry), "soft-state entry expired");
}

static void test_delta_oversized_reconstruction(void)
{
    printf("\n=== RML oversized delta reconstruction ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    uint8_t full[RML_THREAD_TEXT_MAX];
    memset(full, 'a', sizeof(full));
    rml_message_t base;
    CHECK(rml_make_chat(0x1234u, 0u, 0x9005u, full, RML_THREAD_TEXT_MAX, &base),
        "full-size base creation");
    base.msg_id = 0x5401u;
    rml_policy_decide(&base, &ctx);

    rml_delta_t delta = make_delta(RML_DELTA_APPEND, 0u, 0u, "b");
    rml_message_t msg;
    CHECK(rml_make_delta(0x1234u, 0u, 0x9005u, 0x5402u, 0x5401u, &delta, &msg),
        "oversize delta creation");
    rml_policy_decision_t d = rml_policy_decide(&msg, &ctx);
    CHECK(!d.accept && d.reason == RML_REASON_DELTA_FAILED, "oversized reconstruction fails safely");
    CHECK(!rml_seen(0x5402u), "failed delta not marked seen");
}

static void test_thread_cache_wrap(void)
{
    printf("\n=== RML thread cache wrap ===\n");
    rml_init();
    rml_context_t ctx = make_context();
    for (uint32_t i = 0u; i < (uint32_t)RML_THREAD_CACHE_SIZE; i++) {
        rml_message_t msg = make_chat_msg(0x6000u + i, 0xA000u + i, "x");
        ctx.now_ms++;
        rml_policy_decide(&msg, &ctx);
    }
    rml_message_t extra = make_chat_msg(0x7000u, 0xB000u, "y");
    ctx.now_ms++;
    rml_policy_decide(&extra, &ctx);

    rml_thread_entry_t entry;
    CHECK(!rml_thread_cache_get(0x1234u, 0xA000u, &entry), "oldest thread evicted after wrap");
    CHECK(rml_thread_cache_get(0x1234u, 0xB000u, &entry), "new thread present after wrap");
    CHECK(entry.msg_id == 0x7000u, "wrapped cache latest msg_id");
}

static void test_chat_defaults(void)
{
    printf("\n=== RML chat defaults ===\n");
    rml_init();
    const uint8_t text[] = "chat";
    rml_message_t msg;
    CHECK(rml_make_chat(0xBEEFu, 0u, 42u, text, 4u, &msg), "chat creation succeeds");
    CHECK(msg.type == RML_TYPE_CHAT, "type chat");
    CHECK(msg.intent == RML_INTENT_CHAT, "broadcast chat intent");
    CHECK(msg.reliability == RML_REL_OPPORTUNISTIC, "opportunistic default");
    CHECK(msg.ttl > 0u, "ttl default nonzero");
}

int main(void)
{
    test_roundtrip();
    test_bad_magic_version();
    test_payload_bounds();
    test_seen_and_ttl();
    test_policy_reasons();
    test_intent_policy();
    test_delta_append();
    test_delta_replace();
    test_delta_delete_range();
    test_delta_missing_prev_and_repair();
    test_delta_oversized_reconstruction();
    test_thread_cache_wrap();
    test_superseding_behavior();
    test_relevance_decay();
    test_soft_state_expiry();
    test_chat_defaults();

    printf("\nRML tests: pass=%u fail=%u\n", (unsigned)s_pass, (unsigned)s_fail);
    return s_fail == 0u ? 0 : 1;
}
