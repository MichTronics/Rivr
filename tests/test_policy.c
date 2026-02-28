/**
 * @file  test_policy.c
 * @brief Unit tests for the P2 policy engine (policy.c).
 *
 * Tests:
 *   1. classify()    — correct traffic-class assignment per pkt_type
 *   2. TTL pass      — TTL within cap → RIVR_POLICY_PASS
 *   3. TTL clamp     — TTL above cap  → RIVR_POLICY_TTL_CLAMP + new value
 *   4. CONTROL bypass— BEACON always PASS regardless of TTL / bucket
 *   5. Token drain   — 4 consecutive CHAT relays pass, 5th is DROP
 *   6. Token refill  — after 1 s the bucket refills one token
 *   7. Bucket hash   — two source IDs that map to the same bucket share tokens
 *
 * Build (from project root):
 *   gcc -O2 -Ifirmware_core \
 *       -DIRAM_ATTR="" \
 *       firmware_core/policy.c \
 *       tests/test_policy.c \
 *       -o /tmp/test_policy && /tmp/test_policy
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "policy.h"    /* firmware_core/ on include path */
#include "protocol.h"  /* PKT_* constants, rivr_pkt_hdr_t */

/* ── Minimal assertion framework ────────────────────────────────────────── */
static uint32_t s_pass = 0;
static uint32_t s_fail = 0;

#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }          \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg),              \
                        __FILE__, __LINE__); s_fail++; }            \
} while (0)

/* ── Helper: build a minimal packet header ───────────────────────────────── */
static rivr_pkt_hdr_t make_hdr(uint8_t pkt_type, uint8_t ttl, uint32_t src_id)
{
    rivr_pkt_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.pkt_type = pkt_type;
    h.ttl      = ttl;
    h.src_id   = src_id;
    return h;
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 1 — policy_classify()
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_classify(void)
{
    printf("\n=== TEST 1: policy_classify ===\n");
    CHECK(policy_classify(PKT_CHAT)      == RIVR_CLASS_CHAT,    "PKT_CHAT → CHAT");
    CHECK(policy_classify(PKT_DATA)      == RIVR_CLASS_BULK,    "PKT_DATA → BULK");
    CHECK(policy_classify(PKT_BEACON)    == RIVR_CLASS_CONTROL, "PKT_BEACON → CONTROL");
    CHECK(policy_classify(PKT_ROUTE_REQ) == RIVR_CLASS_CONTROL, "PKT_ROUTE_REQ → CONTROL");
    CHECK(policy_classify(PKT_ROUTE_RPL) == RIVR_CLASS_CONTROL, "PKT_ROUTE_RPL → CONTROL");
    CHECK(policy_classify(PKT_ACK)       == RIVR_CLASS_CONTROL, "PKT_ACK → CONTROL");
    CHECK(policy_classify(PKT_PROG_PUSH) == RIVR_CLASS_CONTROL, "PKT_PROG_PUSH → CONTROL");
    CHECK(policy_classify(0xFFu)         == RIVR_CLASS_CONTROL, "unknown → CONTROL");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 2 — TTL within cap → PASS
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_ttl_pass(void)
{
    printf("\n=== TEST 2: TTL within cap ===\n");
    policy_state_t ps;
    policy_init(&ps);
    uint32_t now = 5000u;
    uint8_t  clamp;

    /* CHAT cap=5: TTL=3 → PASS */
    rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 3u, 0x1111u);
    clamp = 99u;
    rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_PASS, "CHAT ttl=3 (cap=5) → PASS");
    CHECK(clamp == 99u,          "clamped_ttl unchanged when PASS");

    /* DATA (BULK) cap=2: TTL=2 → PASS */
    h = make_hdr(PKT_DATA, 2u, 0x2222u);
    clamp = 99u;
    v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_PASS, "DATA ttl=2 (cap=2) → PASS");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 3 — TTL above cap → TTL_CLAMP
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_ttl_clamp(void)
{
    printf("\n=== TEST 3: TTL above cap ===\n");
    policy_state_t ps;
    policy_init(&ps);
    uint32_t now = 5000u;
    uint8_t  clamp;

    /* CHAT cap=5: TTL=7 → TTL_CLAMP, clamped=5 */
    rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 7u, 0x3333u);
    clamp = 99u;
    rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_TTL_CLAMP,    "CHAT ttl=7 (cap=5) → TTL_CLAMP");
    CHECK(clamp == POLICY_TTL_CHAT,      "clamped_ttl == POLICY_TTL_CHAT");

    /* DATA (BULK) cap=2: TTL=5 → TTL_CLAMP, clamped=2 */
    h = make_hdr(PKT_DATA, 5u, 0x4444u);
    clamp = 99u;
    v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_TTL_CLAMP,    "DATA ttl=5 (cap=2) → TTL_CLAMP");
    CHECK(clamp == POLICY_TTL_BULK,      "clamped_ttl == POLICY_TTL_BULK");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 4 — CONTROL always PASS (no bucket, no TTL cap)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_control_bypass(void)
{
    printf("\n=== TEST 4: CONTROL bypass ===\n");
    policy_state_t ps;
    policy_init(&ps);
    uint32_t now = 5000u;
    uint8_t  clamp;

    rivr_pkt_hdr_t h = make_hdr(PKT_BEACON, 7u, 0x5555u);
    clamp = 99u;
    rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_PASS, "BEACON ttl=7 → PASS (no TTL cap)");
    CHECK(clamp == 99u,          "clamped_ttl unchanged for CONTROL");

    /* Exhaust bucket slot for src_id 0x5555 by sending BULK packets */
    for (int i = 0; i < (int)POLICY_MAX_TOKENS + 4; i++) {
        rivr_pkt_hdr_t hb = make_hdr(PKT_CHAT, 1u, 0x5555u);
        policy_check(&ps, &hb, now, &clamp);
    }
    /* Bucket for 0x5555 is now exhausted; BEACON must still PASS */
    h = make_hdr(PKT_BEACON, 3u, 0x5555u);
    v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_PASS, "BEACON passes even after bucket exhausted");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 5 — Token drain: burst of POLICY_MAX_TOKENS+1 should end in DROP
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_token_drain(void)
{
    printf("\n=== TEST 5: Token drain ===\n");
    policy_state_t ps;
    policy_init(&ps);
    uint32_t now  = 5000u;
    uint8_t  clamp;
    uint32_t src  = 0x6666u;

    /* First POLICY_MAX_TOKENS relays must succeed */
    uint32_t pass_cnt = 0u;
    uint32_t drop_cnt = 0u;
    for (uint32_t i = 0; i < POLICY_MAX_TOKENS; i++) {
        rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 1u, src);
        rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
        if (v != RIVR_POLICY_DROP) pass_cnt++;
        else                       drop_cnt++;
    }
    CHECK(pass_cnt == POLICY_MAX_TOKENS, "first MAX_TOKENS relays pass");
    CHECK(drop_cnt == 0u,               "no drops within burst limit");

    /* Next relay must be DROP */
    rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 1u, src);
    rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_DROP, "relay after burst exhausted → DROP");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 6 — Token refill: after 1 s, one token is restored
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_token_refill(void)
{
    printf("\n=== TEST 6: Token refill ===\n");
    policy_state_t ps;
    policy_init(&ps);
    uint32_t now = 10000u;
    uint8_t  clamp;
    uint32_t src = 0x7777u;

    /* Drain all tokens */
    for (uint32_t i = 0; i < POLICY_MAX_TOKENS; i++) {
        rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 1u, src);
        policy_check(&ps, &h, now, &clamp);
    }

    /* Confirm exhausted */
    rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 1u, src);
    rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_DROP, "exhausted bucket → DROP before refill");

    /* Advance time by 1 s → one token refill */
    now += 1000u;
    h = make_hdr(PKT_CHAT, 1u, src);
    v = policy_check(&ps, &h, now, &clamp);
    CHECK(v != RIVR_POLICY_DROP, "after 1 s refill, one relay passes");

    /* Exhausted again immediately */
    h = make_hdr(PKT_CHAT, 1u, src);
    v = policy_check(&ps, &h, now, &clamp);
    CHECK(v == RIVR_POLICY_DROP, "exhausted again after using refill token");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 7 — Bucket hash sharing: two src_ids in same bucket contend
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_bucket_hash(void)
{
    printf("\n=== TEST 7: Bucket hash sharing ===\n");
    policy_state_t ps;
    policy_init(&ps);
    uint32_t now = 20000u;
    uint8_t  clamp;

    /* Find two src_ids that hash to the same bucket */
    uint32_t src_a = 0x00u;   /* bucket 0 */
    uint32_t src_b = (uint32_t)POLICY_BUCKET_COUNT;  /* also bucket 0 */

    CHECK((src_a % POLICY_BUCKET_COUNT) == (src_b % POLICY_BUCKET_COUNT),
          "src_a and src_b share the same bucket slot");

    /* Together they should exhaust the shared bucket in MAX_TOKENS relays */
    uint32_t pass_cnt = 0u;
    for (uint32_t i = 0; i < POLICY_MAX_TOKENS; i++) {
        /* alternate between the two sources */
        uint32_t src = (i % 2u == 0u) ? src_a : src_b;
        rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 1u, src);
        rivr_policy_verdict_t v = policy_check(&ps, &h, now, &clamp);
        if (v != RIVR_POLICY_DROP) pass_cnt++;
    }
    CHECK(pass_cnt == POLICY_MAX_TOKENS, "MAX_TOKENS shared relays all pass");

    /* Now any relay from either source should be dropped */
    rivr_pkt_hdr_t h = make_hdr(PKT_CHAT, 1u, src_a);
    rivr_policy_verdict_t va = policy_check(&ps, &h, now, &clamp);
    h = make_hdr(PKT_CHAT, 1u, src_b);
    rivr_policy_verdict_t vb = policy_check(&ps, &h, now, &clamp);
    CHECK(va == RIVR_POLICY_DROP && vb == RIVR_POLICY_DROP,
          "both sources dropped after shared bucket exhausted");
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== RIVR policy engine unit tests ===\n");
    test_classify();
    test_ttl_pass();
    test_ttl_clamp();
    test_control_bypass();
    test_token_drain();
    test_token_refill();
    test_bucket_hash();

    printf("\n%u PASS  %u FAIL\n", (unsigned)s_pass, (unsigned)s_fail);
    return (s_fail == 0u) ? 0 : 1;
}
