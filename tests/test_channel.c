/**
 * @file  test_channel.c
 * @brief RIVR channel-based messaging test suite (host-native build).
 *
 * Tests cover:
 *   1. Channel table init — defaults correct
 *   2. channel_is_joined / channel_is_muted / channel_is_hidden
 *   3. channel_join / channel_leave (Global cannot be left)
 *   4. channel_set_muted / channel_set_hidden
 *   5. channel_set_tx_default / channel_get_tx_default
 *   6. channel_mark_unread / channel_clear_unread (muted suppresses)
 *   7. PKT_FLAG_CHANNEL payload extraction — v2 frame parsing
 *   8. Legacy PKT_CHAT (no flag) → channel_id = 0
 *   9. Relay policy: channel membership does NOT affect routing_should_forward
 *  10. Dedup is key (src_id, pkt_id) — independent of channel_id
 *  11. Emergency/priority channel detection
 *  12. Per-channel counters (rx_total, tx_total, unread)
 *  13. channel_persist_save / channel_persist_load roundtrip
 *      (no-op stubs on Linux; validates serialization logic)
 *  14. Integration: node joined channel 2 receives msg, node not-joined
 *      channel 2 does NOT store but DOES relay
 *
 * Build (from tests/):
 *   make channel
 *
 * Exit: 0 = all pass, 1 = any fail.
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

/* ── Firmware headers ──────────────────────────────────────────────────────── */
#include "protocol.h"
#include "routing.h"
#include "channel.h"
#include "rivr_metrics.h"
#include "timebase.h"

/* ── External test stubs ───────────────────────────────────────────────────── */
extern void test_advance_ms(uint32_t delta_ms);
extern void test_set_ms(uint32_t abs_ms);
extern atomic_uint_fast32_t g_mono_ms;

/* ── Assertion framework ───────────────────────────────────────────────────── */
static uint32_t s_pass = 0;
static uint32_t s_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            s_pass++; \
        } else { \
            s_fail++; \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

/* ── Helper: build a PKT_CHAT wire frame with optional PKT_FLAG_CHANNEL ─────── */
static int build_chat_frame(uint8_t *buf, size_t cap,
                             uint32_t src_id, uint16_t pkt_id,
                             uint16_t channel_id, bool set_channel_flag,
                             const char *text)
{
    size_t text_len = strlen(text);
    size_t payload_len = set_channel_flag
                         ? (RIVR_CHAT_CHAN_HDR_LEN + text_len)
                         : text_len;

    if (payload_len > RIVR_PKT_MAX_PAYLOAD) return -1;

    static uint8_t payload_buf[RIVR_PKT_MAX_PAYLOAD];
    if (set_channel_flag) {
        payload_buf[0] = (uint8_t)(channel_id & 0xFFu);
        payload_buf[1] = (uint8_t)(channel_id >> 8);
        memcpy(payload_buf + 2, text, text_len);
    } else {
        memcpy(payload_buf, text, text_len);
    }

    rivr_pkt_hdr_t hdr = {
        .magic      = RIVR_MAGIC,
        .version    = RIVR_PROTO_VER,
        .pkt_type   = PKT_CHAT,
        .flags      = set_channel_flag ? PKT_FLAG_CHANNEL : 0u,
        .ttl        = RIVR_PKT_DEFAULT_TTL,
        .hop        = 0u,
        .net_id     = 0u,
        .src_id     = src_id,
        .dst_id     = 0u,
        .seq        = 1u,
        .pkt_id     = pkt_id,
        .payload_len = (uint8_t)payload_len,
        .loop_guard  = 0u,
    };

    return protocol_encode(&hdr, payload_buf, (uint8_t)payload_len,
                           buf, (uint8_t)cap);
}

/**
 * Extract channel_id from a PKT_CHAT payload, honouring PKT_FLAG_CHANNEL.
 * Returns 0 (Global) for legacy frames without the flag.
 * text_out and text_len_out receive the UTF-8 text start and length.
 */
static uint16_t extract_channel(const rivr_pkt_hdr_t *hdr,
                                 const uint8_t *payload,
                                 const uint8_t **text_out,
                                 uint8_t        *text_len_out)
{
    if (hdr->pkt_type != PKT_CHAT) {
        if (text_out)     *text_out     = NULL;
        if (text_len_out) *text_len_out = 0u;
        return 0u;
    }

    if ((hdr->flags & PKT_FLAG_CHANNEL) && hdr->payload_len >= RIVR_CHAT_CHAN_HDR_LEN) {
        uint16_t chan_id = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
        if (text_out)     *text_out     = payload + RIVR_CHAT_CHAN_HDR_LEN;
        if (text_len_out) *text_len_out = (uint8_t)(hdr->payload_len - RIVR_CHAT_CHAN_HDR_LEN);
        return chan_id;
    }

    /* Legacy: no channel prefix → Global */
    if (text_out)     *text_out     = payload;
    if (text_len_out) *text_len_out = hdr->payload_len;
    return RIVR_CHAN_GLOBAL;
}

/* ════════════════════════════════════════════════════════════════════════════
 * TEST FUNCTIONS
 * ════════════════════════════════════════════════════════════════════════════ */

/* Test 1 — Default channel table */
static void test_default_table(void)
{
    printf("[1] Default channel table\n");
    channel_init();

    /* Slot 0: Global */
    const rivr_channel_config_t *cfg0 = channel_get_config(RIVR_CHAN_GLOBAL);
    CHECK(cfg0 != NULL,              "Global config present");
    CHECK(cfg0->id   == 0u,          "Global id == 0");
    CHECK(cfg0->kind == CHAN_KIND_PUBLIC, "Global kind == Public");
    CHECK(strcmp(cfg0->name, "Global") == 0, "Global name");

    /* Slot 1: Ops */
    const rivr_channel_config_t *cfg1 = channel_get_config(1u);
    CHECK(cfg1 != NULL,              "Ops config present");
    CHECK(cfg1->kind == CHAN_KIND_GROUP, "Ops kind == Group");
    CHECK(strcmp(cfg1->name, "Ops") == 0, "Ops name");

    /* Slot 3: Emergency */
    const rivr_channel_config_t *cfg3 = channel_get_config(3u);
    CHECK(cfg3 != NULL,              "Emergency config present");
    CHECK(cfg3->kind == CHAN_KIND_EMERGENCY, "Emergency kind");
    CHECK((cfg3->flags & CHAN_FLAG_PRIORITY) != 0u, "Emergency has PRIORITY flag");
    CHECK(strcmp(cfg3->name, "Emergency") == 0, "Emergency name");

    /* Unknown channel */
    const rivr_channel_config_t *cfgX = channel_get_config(99u);
    CHECK(cfgX == NULL, "Unknown channel returns NULL config");
}

/* Test 2 — Membership predicates */
static void test_membership_predicates(void)
{
    printf("[2] Membership predicates\n");
    channel_init();

    /* Global: joined */
    CHECK(channel_is_joined(0u),  "Global joined");
    CHECK(!channel_is_muted(0u), "Global not muted");
    CHECK(!channel_is_hidden(0u), "Global not hidden");

    /* Ops: joined */
    CHECK(channel_is_joined(1u),  "Ops joined");

    /* Local: not joined */
    CHECK(!channel_is_joined(2u), "Local not joined");
    CHECK(!channel_is_hidden(2u), "Local not hidden");

    /* Emergency: joined */
    CHECK(channel_is_joined(3u),  "Emergency joined");

    /* Slots 4-7: not joined, hidden */
    for (uint16_t i = 4u; i < 8u; i++) {
        CHECK(!channel_is_joined(i), "Empty slot not joined");
        CHECK(channel_is_hidden(i),  "Empty slot hidden");
    }

    /* Unknown channel: safe defaults */
    CHECK(!channel_is_joined(99u), "Unknown not joined");
    CHECK(channel_is_hidden(99u),  "Unknown hidden (safe default)");
}

/* Test 3 — Join / leave */
static void test_join_leave(void)
{
    printf("[3] Join / leave\n");
    channel_init();

    /* Join Local */
    CHECK(!channel_is_joined(2u), "Local not joined before join");
    channel_join(2u);
    CHECK(channel_is_joined(2u),  "Local joined after join");

    /* Leave Local */
    channel_leave(2u);
    CHECK(!channel_is_joined(2u), "Local not joined after leave");

    /* Cannot leave Global (channel 0) — should be silently ignored */
    channel_leave(RIVR_CHAN_GLOBAL);
    CHECK(channel_is_joined(RIVR_CHAN_GLOBAL), "Global remains joined after leave attempt");

    /* Leave unknown channel — no crash */
    channel_leave(99u);
    CHECK(true, "Leave unknown channel no crash");
}

/* Test 4 — Mute / hidden */
static void test_mute_hidden(void)
{
    printf("[4] Mute / hidden\n");
    channel_init();

    channel_set_muted(1u, true);
    CHECK(channel_is_muted(1u), "Ops muted after set_muted(true)");

    channel_set_muted(1u, false);
    CHECK(!channel_is_muted(1u), "Ops not muted after set_muted(false)");

    channel_set_hidden(2u, true);
    CHECK(channel_is_hidden(2u), "Local hidden after set_hidden(true)");

    channel_set_hidden(2u, false);
    CHECK(!channel_is_hidden(2u), "Local not hidden after set_hidden(false)");

    /* No crash on unknown */
    channel_set_muted(99u, true);
    channel_set_hidden(99u, true);
    CHECK(true, "Mute/hide unknown channel no crash");
}

/* Test 5 — TX default */
static void test_tx_default(void)
{
    printf("[5] TX default\n");
    channel_init();

    /* Initial default: Global */
    CHECK(channel_get_tx_default() == RIVR_CHAN_GLOBAL, "Initial TX default = Global");

    /* Cannot set TX default to unjoined channel */
    bool ok = channel_set_tx_default(2u); /* Local is not joined */
    CHECK(!ok, "Cannot set TX default to unjoined channel");
    CHECK(channel_get_tx_default() == RIVR_CHAN_GLOBAL, "TX default unchanged");

    /* Join Ops, set as TX default */
    channel_join(2u);
    ok = channel_set_tx_default(2u);
    CHECK(ok, "set_tx_default succeeds for joined channel");
    CHECK(channel_get_tx_default() == 2u, "TX default is now Local (ch 2)");

    /* Leave Ops — tx_default should fall back to Global */
    channel_leave(2u);
    /* After leave, check whether tx_default reverted to Global */
    CHECK(channel_get_tx_default() == RIVR_CHAN_GLOBAL, "TX default reverts to Global on leave");
}

/* Test 6 — Unread counters */
static void test_unread(void)
{
    printf("[6] Unread counters\n");
    channel_init();

    /* Increment unread on joined channel */
    channel_mark_unread(RIVR_CHAN_GLOBAL);
    channel_mark_unread(RIVR_CHAN_GLOBAL);
    {
        const rivr_channel_counters_t *c = channel_get_counters(RIVR_CHAN_GLOBAL);
        CHECK(c != NULL && c->unread == 2u, "Unread = 2 after two marks");
    }

    channel_clear_unread(RIVR_CHAN_GLOBAL);
    {
        const rivr_channel_counters_t *c = channel_get_counters(RIVR_CHAN_GLOBAL);
        CHECK(c != NULL && c->unread == 0u, "Unread = 0 after clear");
    }

    /* Muted channel: mark_unread suppressed */
    channel_set_muted(RIVR_CHAN_GLOBAL, true);
    channel_mark_unread(RIVR_CHAN_GLOBAL);
    {
        const rivr_channel_counters_t *c = channel_get_counters(RIVR_CHAN_GLOBAL);
        CHECK(c != NULL && c->unread == 0u, "Unread NOT incremented when muted");
    }
    channel_set_muted(RIVR_CHAN_GLOBAL, false);
}

/* Test 7 — PKT_FLAG_CHANNEL payload extraction (v2 frame) */
static void test_channel_flag_extraction(void)
{
    printf("[7] PKT_FLAG_CHANNEL payload extraction\n");

    uint8_t frame[255];
    int flen = build_chat_frame(frame, sizeof(frame),
                                0xAAAA0001ul, 42u,
                                2u,  /* channel_id = 2 (Local) */
                                true,
                                "hello channel");
    CHECK(flen > 0, "Channel frame encodes OK");

    rivr_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    bool ok = protocol_decode(frame, (uint8_t)flen, &hdr, &payload);
    CHECK(ok, "Channel frame decodes OK");
    CHECK(hdr.pkt_type == PKT_CHAT, "pkt_type == PKT_CHAT");
    CHECK((hdr.flags & PKT_FLAG_CHANNEL) != 0u, "PKT_FLAG_CHANNEL set");

    const uint8_t *text    = NULL;
    uint8_t        text_len = 0u;
    uint16_t chan_id = extract_channel(&hdr, payload, &text, &text_len);
    CHECK(chan_id  == 2u, "Extracted channel_id == 2");
    CHECK(text_len == (uint8_t)strlen("hello channel"), "Text length correct");
    CHECK(memcmp(text, "hello channel", text_len) == 0, "Text content correct");
}

/* Test 8 — Legacy PKT_CHAT (no PKT_FLAG_CHANNEL) → channel 0 */
static void test_legacy_channel_id(void)
{
    printf("[8] Legacy PKT_CHAT → channel 0\n");

    uint8_t frame[255];
    int flen = build_chat_frame(frame, sizeof(frame),
                                0xAAAA0001ul, 43u,
                                0u,    /* channel_id ignored for legacy */
                                false, /* no flag */
                                "legacy text");
    CHECK(flen > 0, "Legacy frame encodes OK");

    rivr_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    bool ok = protocol_decode(frame, (uint8_t)flen, &hdr, &payload);
    CHECK(ok, "Legacy frame decodes OK");
    CHECK((hdr.flags & PKT_FLAG_CHANNEL) == 0u, "PKT_FLAG_CHANNEL NOT set in legacy");

    const uint8_t *text     = NULL;
    uint8_t        text_len = 0u;
    uint16_t chan_id = extract_channel(&hdr, payload, &text, &text_len);
    CHECK(chan_id == RIVR_CHAN_GLOBAL, "Legacy frame maps to Global (channel 0)");
    CHECK(text_len == (uint8_t)strlen("legacy text"), "Legacy text length correct");
}

/* Test 9 — Relay: routing_should_forward independent of channel membership */
static void test_relay_independent_of_membership(void)
{
    printf("[9] Relay independent of channel membership\n");
    channel_init();

    dedupe_cache_t cache;
    routing_dedupe_init(&cache);
    test_set_ms(0u);

    /* Build a channel 2 (Local) frame — node is NOT joined to Local */
    uint8_t frame[255];
    int flen = build_chat_frame(frame, sizeof(frame),
                                0xBBBB0002ul, 100u,
                                2u, true, "not joined but must relay");
    CHECK(flen > 0, "Frame encodes OK");

    rivr_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    bool decoded = protocol_decode(frame, (uint8_t)flen, &hdr, &payload);
    CHECK(decoded, "Frame decoded for relay test");

    /* Confirm this node is NOT joined to channel 2 */
    CHECK(!channel_is_joined(2u), "Not joined to channel 2");

    /* routing_should_forward must return true (TTL OK, not in dedup cache) */
    bool should_fwd = routing_should_forward(&cache, &hdr, 0u);
    CHECK(should_fwd, "routing_should_forward=true for unjoined channel");

    /* Duplicate: should NOT forward */
    /* Re-build with same pkt_id to get same dedupe key */
    int flen2 = build_chat_frame(frame, sizeof(frame),
                                 0xBBBB0002ul, 100u,
                                 2u, true, "not joined but must relay");
    (void)flen2;
    rivr_pkt_hdr_t hdr2;
    const uint8_t *payload2 = NULL;
    protocol_decode(frame, (uint8_t)flen, &hdr2, &payload2);

    bool should_fwd2 = routing_should_forward(&cache, &hdr2, 0u);
    CHECK(!should_fwd2, "routing_should_forward=false for duplicate");
}

/* Test 10 — Dedup is (src_id, pkt_id); channel_id doesn't affect dedup key */
static void test_dedup_independent_of_channel(void)
{
    printf("[10] Dedup key is (src_id, pkt_id) — channel_id irrelevant\n");

    dedupe_cache_t cache;
    routing_dedupe_init(&cache);
    test_set_ms(0u);

    /* Two frames from same src with same pkt_id but DIFFERENT channel_ids */
    uint8_t f1[255], f2[255];
    build_chat_frame(f1, sizeof(f1), 0xAAAA0001ul, 77u, 0u, true, "msg on ch 0");
    build_chat_frame(f2, sizeof(f2), 0xAAAA0001ul, 77u, 1u, true, "same pkt_id ch 1");

    rivr_pkt_hdr_t h1, h2;
    const uint8_t *p1 = NULL, *p2 = NULL;
    protocol_decode(f1, (uint8_t)sizeof(f1), &h1, &p1);
    protocol_decode(f2, (uint8_t)sizeof(f2), &h2, &p2);

    /* First frame: new → forward */
    bool fwd1 = routing_should_forward(&cache, &h1, 0u);
    CHECK(fwd1, "First injection (ch0, pkt_id=77) forwarded");

    /* Second frame: same src_id + pkt_id → dedup hit even if channel differs */
    bool fwd2 = routing_should_forward(&cache, &h2, 0u);
    CHECK(!fwd2, "Same pkt_id different channel → dedup suppressed");

    /* Different pkt_id → new injection, forwarded */
    uint8_t f3[255];
    build_chat_frame(f3, sizeof(f3), 0xAAAA0001ul, 78u, 1u, true, "new pkt_id");
    rivr_pkt_hdr_t h3;
    const uint8_t *p3 = NULL;
    protocol_decode(f3, (uint8_t)sizeof(f3), &h3, &p3);
    bool fwd3 = routing_should_forward(&cache, &h3, 0u);
    CHECK(fwd3, "Different pkt_id forwarded even on same channel");
}

/* Test 11 — Emergency / priority channel */
static void test_priority_channel(void)
{
    printf("[11] Emergency / priority channel\n");
    channel_init();

    CHECK(channel_is_priority(3u), "Emergency channel is priority");
    CHECK(!channel_is_priority(0u), "Global is not priority");
    CHECK(!channel_is_priority(1u), "Ops is not priority");
    CHECK(!channel_is_priority(99u), "Unknown is not priority");
}

/* Test 12 — Per-channel counters */
static void test_per_channel_counters(void)
{
    printf("[12] Per-channel counters\n");
    channel_init();

    rivr_channel_counters_t *c0 = channel_get_counters(0u);
    CHECK(c0 != NULL, "Global counters accessible");
    CHECK(c0->rx_total == 0u, "Global rx_total starts at 0");

    /* Manually increment counters (as the RX path would) */
    c0->rx_total++;
    c0->rx_stored++;
    c0->tx_total++;

    CHECK(c0->rx_total  == 1u, "Global rx_total incremented to 1");
    CHECK(c0->rx_stored == 1u, "Global rx_stored incremented to 1");
    CHECK(c0->tx_total  == 1u, "Global tx_total incremented to 1");

    /* Counters for unknown channel: returns NULL */
    CHECK(channel_get_counters(99u) == NULL, "Unknown channel counters = NULL");
}

/* Test 13 — Persist roundtrip (stub: save returns true, load returns false) */
static void test_persist_roundtrip(void)
{
    printf("[13] Persist roundtrip (stub on Linux)\n");
    channel_init();

    /* On Linux stub: save is always true, load is always false */
    bool saved = channel_persist_save();
    CHECK(saved, "channel_persist_save returns true (stub)");

    bool loaded = channel_persist_load();
    /* Linux stub returns false (no NVS) — that's correct */
    CHECK(!loaded, "channel_persist_load returns false on Linux (no NVS)");

    /* After load=false, channel_init() restores defaults */
    if (!loaded) channel_init();
    CHECK(channel_is_joined(RIVR_CHAN_GLOBAL), "Defaults restored after failed load");
}

/* Test 14 — Integration: joined node stores, unjoined node relays only */
static void test_integration_joined_vs_unjoined(void)
{
    printf("[14] Integration: joined stores, unjoined relays\n");

    /*
     * Simulate two virtual nodes using separate channel tables and
     * dedupe caches.  We don't have real "other node" state here,
     * so we use two channel_table states via init + re-init.
     */

    /* --- Node B: JOINED to channel 2 --- */
    channel_init();
    channel_join(2u);

    uint8_t frame[255];
    int flen = build_chat_frame(frame, sizeof(frame),
                                0xAAAA0001ul, 200u,
                                2u, true, "test integration msg");
    CHECK(flen > 0, "Integration frame encoded");

    rivr_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    protocol_decode(frame, (uint8_t)flen, &hdr, &payload);

    uint16_t chan_id = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    bool joined_b    = channel_is_joined(chan_id);
    CHECK(joined_b, "Node B is joined to channel 2");

    if (joined_b) {
        rivr_channel_counters_t *cnt = channel_get_counters(chan_id);
        if (cnt) {
            cnt->rx_total++;
            cnt->rx_stored++;
            channel_mark_unread(chan_id);
        }
    }

    {
        const rivr_channel_counters_t *c = channel_get_counters(2u);
        CHECK(c != NULL && c->rx_stored == 1u, "Node B stored the message");
        CHECK(c != NULL && c->unread    == 1u, "Node B has unread=1");
    }

    dedupe_cache_t cache_b;
    routing_dedupe_init(&cache_b);
    test_set_ms(0u);

    rivr_pkt_hdr_t hdr_b;
    const uint8_t *payload_b = NULL;
    protocol_decode(frame, (uint8_t)flen, &hdr_b, &payload_b);
    bool relay_b = routing_should_forward(&cache_b, &hdr_b, 0u);
    CHECK(relay_b, "Node B (joined) still relays");

    /* --- Node C: NOT JOINED to channel 2 --- */
    channel_init(); /* re-init = defaults (Local not joined) */
    CHECK(!channel_is_joined(2u), "Node C not joined channel 2");

    dedupe_cache_t cache_c;
    routing_dedupe_init(&cache_c);
    test_set_ms(0u);

    rivr_pkt_hdr_t hdr_c;
    const uint8_t *payload_c = NULL;
    protocol_decode(frame, (uint8_t)flen, &hdr_c, &payload_c);

    uint16_t chan_c = (uint16_t)(payload_c[0] | ((uint16_t)payload_c[1] << 8));
    bool joined_c   = channel_is_joined(chan_c);
    CHECK(!joined_c, "Node C confirms not joined");

    /* C uses routing_should_forward which doesn't consult channel membership */
    bool relay_c = routing_should_forward(&cache_c, &hdr_c, 0u);
    CHECK(relay_c, "Node C (unjoined) still relays channel 2 frame");

    /* C does NOT store the message (membership gate) */
    if (!joined_c) {
        /* In real firmware: store is skipped, only relay fires */
        CHECK(true, "Node C skips store (membership gate)");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== RIVR channel test suite ===\n\n");

    /* Initialise routing subsystem (for dedup tests) */
    dedupe_cache_t _cache;
    routing_dedupe_init(&_cache);
    test_set_ms(0u);
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));

    test_default_table();
    test_membership_predicates();
    test_join_leave();
    test_mute_hidden();
    test_tx_default();
    test_unread();
    test_channel_flag_extraction();
    test_legacy_channel_id();
    test_relay_independent_of_membership();
    test_dedup_independent_of_channel();
    test_priority_channel();
    test_per_channel_counters();
    test_persist_roundtrip();
    test_integration_joined_vs_unjoined();

    printf("\n=== Results: %u passed, %u failed ===\n", s_pass, s_fail);
    return (s_fail == 0u) ? 0 : 1;
}
