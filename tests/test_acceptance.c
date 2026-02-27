/**
 * @file  test_acceptance.c
 * @brief 3-run A→D acceptance test suite (host-native build, no ESP-IDF).
 *
 * RUN 1 — Flood correctness
 *   50 unique CHAT packets → all forwarded, TTL/hop mutated, relay flag set.
 *   Same 50 replayed      → all deduped (DEDUPE-DROP).
 *   Jitter distribution   → spread covers low and high end of [0..MAX_MS].
 *
 * RUN 2 — Hybrid route (cache miss → pending → RPL → drain)
 *   Cache miss for NODE_D  → RCACHE_TX_FLOOD.
 *   Enqueue pending frame  → count == 1.
 *   route_cache_update()   → cache hit, next_hop == NODE_B.
 *   drain_for_dst()        → 1 frame delivered, count == 0.
 *   Popped frame           → dst_id == NODE_B, ttl == 1.
 *
 * RUN 3 — Congestion + due_ms gate (no head-of-line blocking)
 *   Fill queue to SPSC capacity  → next push fails, drops counted.
 *   Fallback frame structure     → PKT_FLAG_FALLBACK, hop==0, no RELAY flag.
 *   due_ms gate (continue, not break) → immediate frames drain past deferred.
 *
 * Exit code: 0 = all checks passed, 1 = at least one check failed.
 *
 * Build (from project root):
 *   See tests/run.ps1
 */

/* ESP-IDF attribute macros — must be defined before any firmware header. */
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

/* Firmware headers (include paths set via -Ifirmware_core) */
#include "protocol.h"
#include "routing.h"
#include "route_cache.h"
#include "pending_queue.h"
#include "radio_sx1262.h"   /* rf_tx_request_t, rb_t, RF_TX_QUEUE_CAP */
#include "timebase.h"       /* tb_millis() via g_mono_ms atom */

/* ── External stubs (defined in test_stubs.c) ──────────────────────────────── */
extern void test_stubs_init(void);
extern void test_advance_ms(uint32_t delta_ms);
extern atomic_uint_fast32_t g_mono_ms;

/* ── Node IDs (same as sim demo) ─────────────────────────────────────────── */
#define MY_NODE  0xFEED0000ul
#define NODE_A   0xAAAA0001ul
#define NODE_B   0xBBBB0002ul
#define NODE_D   0xDDDD0004ul

/* ── Minimal assertion framework ─────────────────────────────────────────── */
static uint32_t s_pass = 0;
static uint32_t s_fail = 0;

#define CHECK(cond, msg) do {                                       \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }           \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg),               \
                        __FILE__, __LINE__); s_fail++; }             \
} while (0)

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 1 — Flood correctness
 * ══════════════════════════════════════════════════════════════════════════ */
static void run1_flood_correctness(void)
{
    /* FLOOD_BATCH < FWDBUDGET_MAX_FWD(30) and < DEDUPE_CACHE_SIZE(32)
     * so neither the budget cap nor ring-wrap interfere with this test. */
#define FLOOD_BATCH 25u
    printf("\n=== RUN 1: Flood Correctness (%u chat pkts) ===\n", (unsigned)FLOOD_BATCH);

    routing_init();
    dedupe_cache_t   *dc = routing_get_dedupe();
    forward_budget_t *fb = routing_get_fwdbudget();

    uint32_t now      = tb_millis();
    uint32_t fwd_ok   = 0u;
    uint32_t ttl_ok   = 0u;
    uint32_t hop_ok   = 0u;
    uint32_t relay_ok = 0u;

    /* ── 1a: FLOOD_BATCH unique packets → all forwarded ─────────────────── */
    for (uint32_t i = 0u; i < FLOOD_BATCH; i++) {
        rivr_pkt_hdr_t pkt = {
            .magic    = RIVR_MAGIC,
            .version  = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT,
            .ttl      = RIVR_PKT_DEFAULT_TTL,
            .hop      = 0u,
            .src_id   = NODE_A,
            .dst_id   = 0u,
            .seq      = i,
        };
        uint8_t init_ttl = pkt.ttl;
        uint8_t init_hop = pkt.hop;
        uint32_t toa = routing_toa_estimate_us(20u);
        rivr_fwd_result_t r = routing_flood_forward(dc, fb, &pkt, toa, now);

        if (r == RIVR_FWD_FORWARD) {
            fwd_ok++;
            if (pkt.ttl == init_ttl - 1u) ttl_ok++;
            if (pkt.hop == init_hop + 1u) hop_ok++;
            if ((pkt.flags & PKT_FLAG_RELAY) != 0u) relay_ok++;
        }
    }

    CHECK(fwd_ok   == FLOOD_BATCH, "FLOOD_BATCH unique packets forwarded");
    CHECK(ttl_ok   == FLOOD_BATCH, "TTL decremented by 1 on every forward");
    CHECK(hop_ok   == FLOOD_BATCH, "hop incremented by 1 on every forward");
    CHECK(relay_ok == FLOOD_BATCH, "PKT_FLAG_RELAY set on every forward");

    /* ── 1b: same FLOOD_BATCH replayed → all deduped ────────────────────── */
    uint32_t deduped = 0u;
    for (uint32_t i = 0u; i < FLOOD_BATCH; i++) {
        rivr_pkt_hdr_t pkt = {
            .magic    = RIVR_MAGIC,
            .version  = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT,
            .ttl      = RIVR_PKT_DEFAULT_TTL,
            .src_id   = NODE_A,
            .dst_id   = 0u,
            .seq      = i,
        };
        uint32_t toa = routing_toa_estimate_us(20u);
        rivr_fwd_result_t r = routing_flood_forward(dc, fb, &pkt, toa, now);
        if (r == RIVR_FWD_DROP_DEDUPE) deduped++;
    }
    CHECK(deduped == FLOOD_BATCH, "FLOOD_BATCH replayed packets deduped (DEDUPE-DROP)");

    /* ── 1c: same (src, seq) via different relay → still deduped ───────── */
    rivr_pkt_hdr_t alias = {
        .magic    = RIVR_MAGIC,
        .version  = RIVR_PROTO_VER,
        .pkt_type = PKT_CHAT,
        .flags    = PKT_FLAG_RELAY,
        .ttl      = RIVR_PKT_DEFAULT_TTL - 1u,
        .hop      = 1u,
        .src_id   = NODE_A,
        .dst_id   = 0u,
        .seq      = 0u,    /* same as first packet in the burst */
    };
    rivr_fwd_result_t ra = routing_flood_forward(
        dc, fb, &alias, routing_toa_estimate_us(20u), now);
    CHECK(ra == RIVR_FWD_DROP_DEDUPE,
        "same (src,seq) via different relay still deduped (GATE2)");

    /* ── 1d: jitter spreads over full [0 .. FORWARD_JITTER_MAX_MS] range ─ */
    uint32_t jmin = UINT32_MAX;
    uint32_t jmax = 0u;
    /* Use 200 different (src_id, seq, pkt_type) seeds to sample the range. */
    for (uint32_t i = 0u; i < 200u; i++) {
        uint32_t d = routing_forward_delay_ms(NODE_A + i * 0x100u,
                                               i * 13u, PKT_CHAT);
        if (d < jmin) jmin = d;
        if (d > jmax) jmax = d;
    }
    CHECK(jmin   <=  20u,               "Jitter min ≤ 20 ms (spread covers low end)");
    CHECK(jmax   >= 150u,               "Jitter max ≥ 150 ms (spread covers high end)");
    CHECK(jmax   <= FORWARD_JITTER_MAX_MS, "Jitter never exceeds FORWARD_JITTER_MAX_MS");

    /* ── 1e: same seed → same jitter (determinism) ──────────────────────── */
    uint32_t d1 = routing_forward_delay_ms(NODE_A, 42u, PKT_CHAT);
    uint32_t d2 = routing_forward_delay_ms(NODE_A, 42u, PKT_CHAT);
    CHECK(d1 == d2, "Jitter is deterministic: same seed → same value");

    /* ── 1f: different pkt_type with same (src,seq) → different delay ───── */
    uint32_t dc_chat = routing_forward_delay_ms(NODE_A, 7u, PKT_CHAT);
    uint32_t dc_data = routing_forward_delay_ms(NODE_A, 7u, PKT_DATA);
    CHECK(dc_chat != dc_data,
        "Different pkt_type produces different jitter (type spread)");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 2 — Hybrid route (cache miss → pending → RPL → drain)
 * ══════════════════════════════════════════════════════════════════════════ */
static void run2_hybrid_route(void)
{
    printf("\n=== RUN 2: Hybrid Route (miss→pending→RPL→drain) ===\n");

    route_cache_t cache;
    route_cache_init(&cache);

    pending_queue_t pq;
    pending_queue_init(&pq);

    uint32_t now     = tb_millis();
    uint32_t next_hop = 0u;

    /* ── 2a: cache miss for unknown destination ───────────────────────── */
    rcache_tx_decision_t dec =
        route_cache_tx_decide(&cache, NODE_D, now, &next_hop);
    CHECK(dec == RCACHE_TX_FLOOD,  "cache miss → RCACHE_TX_FLOOD");
    CHECK(next_hop == 0u,           "next_hop == 0 on cache miss");

    /* ── 2b: enqueue pending frame for NODE_D ──────────────────────────── */
    rivr_pkt_hdr_t ph = {
        .magic    = RIVR_MAGIC,    .version  = RIVR_PROTO_VER,
        .pkt_type = PKT_CHAT,      .ttl      = RIVR_PKT_DEFAULT_TTL,
        .src_id   = MY_NODE,       .dst_id   = NODE_D,       .seq = 42u,
    };
    uint8_t wire[64];
    int wlen = protocol_encode(&ph, (const uint8_t *)"test-payload", 12u,
                                wire, sizeof(wire));
    CHECK(wlen > 0, "protocol_encode for pending frame succeeds");

    bool pq_ok = pending_queue_enqueue(
        &pq, NODE_D, wire, (uint8_t)wlen,
        RF_TOA_APPROX_US((uint8_t)wlen), now);
    CHECK(pq_ok,                             "pending_queue_enqueue succeeds");
    CHECK(pending_queue_count(&pq) == 1u,    "pending count == 1 after enqueue");

    /* ── 2c: simulate ROUTE_RPL → update route cache ──────────────────── */
    route_cache_update(&cache, NODE_D, NODE_B,
                       /*hops*/2u, /*metric*/170u,
                       RCACHE_FLAG_VALID, now);

    dec = route_cache_tx_decide(&cache, NODE_D, now, &next_hop);
    CHECK(dec == RCACHE_TX_UNICAST, "after route_cache_update → RCACHE_TX_UNICAST");
    CHECK(next_hop == NODE_B,        "next_hop == NODE_B");

    /* ── 2d: drain pending queue into a local TX queue ─────────────────── */
    static rf_tx_request_t tx_storage[8];
    static rb_t tx_q;
    rb_init(&tx_q, tx_storage, 8u, sizeof(rf_tx_request_t));

    uint8_t drained = pending_queue_drain_for_dst(&pq, NODE_D, NODE_B,
                                                   &tx_q, now);
    CHECK(drained == 1u,                       "drain_for_dst returns 1");
    CHECK(pending_queue_count(&pq) == 0u,      "pending count == 0 after drain");
    CHECK(rb_available(&tx_q) == 1u,           "TX queue has exactly 1 frame");

    /* ── 2e: pop and verify the rewritten frame ─────────────────────────── */
    rf_tx_request_t req;
    bool popped = rb_pop(&tx_q, &req);
    CHECK(popped, "rb_pop succeeds");

    rivr_pkt_hdr_t out_hdr;
    const uint8_t *out_pl = NULL;
    bool decoded = protocol_decode(req.data, req.len, &out_hdr, &out_pl);
    CHECK(decoded,                             "drained frame decodes with valid CRC");
    CHECK(out_hdr.dst_id  == NODE_B,           "drained dst_id == next_hop (NODE_B)");
    CHECK(out_hdr.ttl     == 1u,               "drained ttl == 1 (single-hop unicast)");
    CHECK(req.due_ms      == 0u,               "drained due_ms == 0 (immediate send)");

    /* ── 2f: second ROUTE_RPL must NOT re-drain already-cleared slot ────── */
    uint8_t second_drain = pending_queue_drain_for_dst(&pq, NODE_D, NODE_B,
                                                        &tx_q, now);
    CHECK(second_drain == 0u,
        "second drain on same dst returns 0 (no double-send)");

    /* ── 2g: entry expires after PENDING_EXPIRY_MS ───────────────────────── */
    /* Re-enqueue, then advance clock past expiry, try to drain. */
    pending_queue_enqueue(&pq, NODE_D, wire, (uint8_t)wlen,
                          RF_TOA_APPROX_US((uint8_t)wlen), now);
    CHECK(pending_queue_count(&pq) == 1u, "re-enqueue for expiry test: count == 1");

    test_advance_ms(PENDING_EXPIRY_MS + 1u);
    uint32_t now2 = tb_millis();
    uint8_t expired_drain = pending_queue_drain_for_dst(&pq, NODE_D, NODE_B,
                                                         &tx_q, now2);
    CHECK(expired_drain == 0u,             "expired entry not delivered (0 drained)");
    CHECK(pending_queue_count(&pq) == 0u,  "pending count == 0 after expiry drain");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 3 — Congestion + due_ms gate
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Mirrors the due_ms gate in tx_drain_loop (main.c) — uses continue, not
 * break, so deferred frames don't block immediately-due frames.
 */
static uint32_t mini_drain(rb_t *q, uint32_t now_ms)
{
    uint32_t drained = 0u;
    for (uint32_t i = 0u; i < 16u; i++) {
        rf_tx_request_t req;
        if (!rb_pop(q, &req)) break;                /* queue empty */

        if (req.due_ms != 0u && req.due_ms > now_ms) {
            rb_try_push(q, &req);                   /* push deferred to tail */
            continue;                               /* skip — no HoL block   */
        }
        drained++;
    }
    return drained;
}

static void run3_congestion_and_due_ms(void)
{
    printf("\n=== RUN 3: Congestion + due_ms gate ===\n");

    /* ── 3a: SPSC queue drops when full ──────────────────────────────────── */
    /* RF_TX_QUEUE_CAP = 4; SPSC leaves 1 slot fallow → max 3 live items.    */
    static rf_tx_request_t storage_a[RF_TX_QUEUE_CAP];
    static rb_t q_a;
    rb_init(&q_a, storage_a, RF_TX_QUEUE_CAP, sizeof(rf_tx_request_t));

    rf_tx_request_t dummy; memset(&dummy, 0, sizeof(dummy));
    dummy.len    = 1u;
    dummy.toa_us = 1000u;

    uint32_t pushed = 0u;
    for (uint32_t i = 0u; i < RF_TX_QUEUE_CAP; i++) {
        if (rb_try_push(&q_a, &dummy)) pushed++;
    }
    /* SPSC: cap=4, max storable = cap-1 = 3 */
    CHECK(pushed == (uint32_t)(RF_TX_QUEUE_CAP - 1u),
        "SPSC queue fills to cap-1 (one slot always fallow)");
    CHECK(rb_try_push(&q_a, &dummy) == false,
        "push to full queue returns false");
    CHECK(rb_drops(&q_a) >= 1u,
        "drop counter incremented after overflow");

    /* ── 3b: PKT_FLAG_FALLBACK frame is well-formed ─────────────────────── */
    rivr_pkt_hdr_t fb_hdr = {
        .magic    = RIVR_MAGIC,            .version = RIVR_PROTO_VER,
        .pkt_type = PKT_CHAT,              .ttl     = RIVR_FALLBACK_TTL,
        .hop      = 0u,                    /* originated, not relay hop     */
        .flags    = PKT_FLAG_FALLBACK,     /* RELAY flag must NOT be set    */
        .src_id   = MY_NODE,               .dst_id  = 0u,       .seq = 99u,
    };
    uint8_t fb_wire[64];
    int fb_len = protocol_encode(&fb_hdr, (const uint8_t *)"fb", 2u,
                                  fb_wire, sizeof(fb_wire));
    CHECK(fb_len > 0, "fallback frame encodes successfully");

    rivr_pkt_hdr_t fb_out;
    const uint8_t *fb_pl = NULL;
    bool fb_ok = protocol_decode(fb_wire, (uint8_t)fb_len, &fb_out, &fb_pl);
    CHECK(fb_ok,                                     "fallback frame decodes (CRC valid)");
    CHECK((fb_out.flags & PKT_FLAG_FALLBACK) != 0u,  "PKT_FLAG_FALLBACK set");
    CHECK((fb_out.flags & PKT_FLAG_RELAY)    == 0u,  "PKT_FLAG_RELAY NOT set (originated)");
    CHECK(fb_out.hop      == 0u,                     "hop == 0 (originated fallback)");
    CHECK(fb_out.ttl      == RIVR_FALLBACK_TTL,      "ttl == RIVR_FALLBACK_TTL");
    CHECK(fb_out.dst_id   == 0u,                     "dst_id == 0 (broadcast)");

    /* ── 3c: due_ms gate — no head-of-line blocking ─────────────────────── */
    /* Push order: deferred frame first, immediate frame second.
     * Without the continue-vs-break fix, the immediate frame would never
     * drain in the same tick.  With the fix, it drains correctly.           */
    static rf_tx_request_t storage_c[8];
    static rb_t q_c;
    rb_init(&q_c, storage_c, 8u, sizeof(rf_tx_request_t));

    uint32_t now = tb_millis();

    rf_tx_request_t deferred; memset(&deferred, 0, sizeof(deferred));
    deferred.due_ms   = now + 500u;   /* 500 ms in the future */
    deferred.len      = 1u;
    deferred.data[0]  = 0xDDu;
    rb_try_push(&q_c, &deferred);

    rf_tx_request_t immediate; memset(&immediate, 0, sizeof(immediate));
    immediate.due_ms  = 0u;           /* send immediately */
    immediate.len     = 1u;
    immediate.data[0] = 0x11u;
    rb_try_push(&q_c, &immediate);

    CHECK(rb_available(&q_c) == 2u, "two frames in queue before drain");

    uint32_t drained = mini_drain(&q_c, now);
    CHECK(drained == 1u,
        "due_ms gate: 1 frame drained (immediate) — no HoL blocking");
    CHECK(rb_available(&q_c) == 1u,
        "deferred frame still in queue after drain");

    /* Verify the remaining frame is the deferred one */
    rf_tx_request_t leftover;
    rb_pop(&q_c, &leftover);
    CHECK(leftover.data[0] == 0xDDu,
        "remaining frame is the deferred one (data[0] == 0xDD)");

    /* ── 3d: deferred frame drains once its time arrives ─────────────────── */
    rb_try_push(&q_c, &deferred);  /* put it back */
    test_advance_ms(600u);          /* advance past due_ms */
    uint32_t now2   = tb_millis();
    uint32_t drained2 = mini_drain(&q_c, now2);
    CHECK(drained2 == 1u,
        "deferred frame drains once now_ms >= due_ms");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * RUN 4 — Link scoring, hysteresis, age decay, route selection
 * ══════════════════════════════════════════════════════════════════════════ */
static void run4_link_scoring(void)
{
    printf("\n=== RUN 4: Link Scoring, Hysteresis, Age Decay ===\n");

    /* ── 4a: Score formula — known RSSI+SNR → expected value ────────────── */
    /* rssi_part = clamp(rssi+140, 0, 80); snr_part = clamp(snr+10, 0, 20)  */
    neighbor_table_t nb;
    routing_neighbor_init(&nb);

    uint32_t now = tb_millis();

    rivr_pkt_hdr_t ph;
    ph.src_id   = NODE_A;
    ph.hop      = 0u;
    ph.seq      = 0u;
    ph.pkt_type = PKT_CHAT;

    /* Excellent link: rssi=-60 dBm, snr=+10 dB → rssi_part=80, snr_part=20 → 100 */
    routing_neighbor_update(&nb, &ph, -60, 10, now);
    const neighbor_entry_t *na = routing_neighbor_get(&nb, 0u);
    CHECK(na != NULL, "4a: NODE_A entry exists after first update");
    uint8_t score_a = routing_neighbor_link_score(na, now);
    CHECK(score_a == 100u, "4a: excellent link (rssi=-60, snr=+10) → score 100");

    /* Mid link: rssi=-100 dBm, snr=0 dB → rssi_part=40, snr_part=10 → 50  */
    ph.src_id = NODE_B;
    routing_neighbor_update(&nb, &ph, -100, 0, now);
    const neighbor_entry_t *nb_ent = routing_neighbor_get(&nb, 1u);
    CHECK(nb_ent != NULL, "4a: NODE_B entry exists");
    uint8_t score_b = routing_neighbor_link_score(nb_ent, now);
    CHECK(score_b == 50u, "4a: mid link (rssi=-100, snr=0) → score 50");

    /* Weak link: rssi=-120 dBm, snr=-10 dB → rssi_part=20, snr_part=0 → 20 */
    ph.src_id = NODE_D;
    routing_neighbor_update(&nb, &ph, -120, -10, now);
    const neighbor_entry_t *nd = routing_neighbor_get(&nb, 2u);
    CHECK(nd != NULL, "4a: NODE_D entry exists");
    uint8_t score_d = routing_neighbor_link_score(nd, now);
    CHECK(score_d == 20u, "4a: weak link (rssi=-120, snr=-10) → score 20");

    /* Score ordering must be consistent */
    CHECK(score_a > score_b && score_b > score_d,
          "4a: score ordering: excellent > mid > weak");

    /* ── 4b: Age decay — same link, clock advanced → score decreases ─────── */
    /* Advance clock to half the expiry window */
    test_advance_ms(NEIGHBOR_EXPIRY_MS / 2u);
    uint32_t now2 = tb_millis();

    /* NODE_A (base=100) should now be ~50 */
    uint8_t score_a_aged = routing_neighbor_link_score(na, now2);
    CHECK(score_a_aged < score_a,
          "4b: score decreases after half-expiry elapsed");
    /* Tolerate ±1 from integer rounding */
    CHECK(score_a_aged >= 49u && score_a_aged <= 51u,
          "4b: aged score ≈ 50 (base/2 at half-expiry window)");

    /* Advance past full expiry → score == 0 */
    test_advance_ms(NEIGHBOR_EXPIRY_MS / 2u + 1u);
    uint32_t now3 = tb_millis();
    uint8_t score_a_expired = routing_neighbor_link_score(na, now3);
    CHECK(score_a_expired == 0u, "4b: score == 0 once past NEIGHBOR_EXPIRY_MS");

    /* ── 4c: Hysteresis — small improvement does NOT swap route ─────────── */
    route_cache_t rc;
    route_cache_init(&rc);
    uint32_t t0 = tb_millis();

    /* Install a route: NODE_D via NODE_B, hops=2, metric=60 */
    route_cache_update(&rc, NODE_D, NODE_B, 2u, 60u, RCACHE_FLAG_VALID, t0);
    /* Attempt upgrade: same hops, metric=68 (delta=8 < RCACHE_METRIC_HYSTERESIS=10) */
    route_cache_update(&rc, NODE_D, NODE_A, 2u, 68u, RCACHE_FLAG_VALID, t0);

    uint32_t nh_out = 0u;
    rcache_tx_decision_t dec = route_cache_tx_decide(&rc, NODE_D, t0, &nh_out);
    CHECK(dec == RCACHE_TX_UNICAST, "4c: route exists (UNICAST)");
    CHECK(nh_out == NODE_B,
          "4c: hysteresis: small improvement (delta=8) does NOT swap next_hop");

    /* ── 4d: Route upgrade — sufficient improvement DOES swap route ──────── */
    /* metric=75 (delta=15 > RCACHE_METRIC_HYSTERESIS=10) → should swap     */
    route_cache_update(&rc, NODE_D, NODE_A, 2u, 75u, RCACHE_FLAG_VALID, t0);
    nh_out = 0u;
    dec = route_cache_tx_decide(&rc, NODE_D, t0, &nh_out);
    CHECK(dec == RCACHE_TX_UNICAST, "4d: route still valid after upgrade attempt");
    CHECK(nh_out == NODE_A,
          "4d: sufficient improvement (delta=15) DOES swap next_hop to NODE_A");

    /* ── 4e: rx_ok counter increments on each neighbor update ───────────── */
    neighbor_table_t nb2;
    routing_neighbor_init(&nb2);
    uint32_t t1 = tb_millis();
    rivr_pkt_hdr_t p2;
    p2.src_id = NODE_B; p2.hop = 0u; p2.seq = 0u; p2.pkt_type = PKT_CHAT;

    routing_neighbor_update(&nb2, &p2, -80, 5, t1);
    routing_neighbor_update(&nb2, &p2, -80, 5, t1 + 100u);
    routing_neighbor_update(&nb2, &p2, -80, 5, t1 + 200u);

    const neighbor_entry_t *nb2_ent = routing_neighbor_get(&nb2, 0u);
    CHECK(nb2_ent != NULL, "4e: entry exists");
    CHECK(nb2_ent->rx_ok == 3u, "4e: rx_ok == 3 after three updates");

    /* ── 4f: EWMA — RSSI smooths toward new value ───────────────────────── */
    /* Initial entry at -80 dBm. Then hit with -60 dBm once.
     * EWMA α=1/8: new = (old*7 + new_sample)/8 = (-80*7 + -60)/8 = -77  */
    routing_neighbor_update(&nb2, &p2, -60, 5, t1 + 300u);
    CHECK(nb2_ent->rssi_dbm == -77,
          "4f: EWMA RSSI smooths: (-80*7 + -60)/8 == -77");
    /* After the 4th update rx_ok should be 4 */
    CHECK(nb2_ent->rx_ok == 4u, "4f: rx_ok == 4 after EWMA update");

    /* ── 4g: route_cache_learn_rx wires SNR into metric ─────────────────── */
    route_cache_t rc2;
    route_cache_init(&rc2);
    uint32_t t2 = tb_millis();
    /* Excellent direct link: rssi=-60, snr=+10 → metric=100                */
    route_cache_learn_rx(&rc2, NODE_A, 0u, 0u, -60, 10, t2);
    const route_cache_entry_t *re = route_cache_lookup(&rc2, NODE_A, t2);
    CHECK(re != NULL, "4g: route learned via route_cache_learn_rx");
    CHECK(re->metric == 100u,
          "4g: metric == 100 for excellent direct link (rssi=-60, snr=+10)");

    /* Weaker path arrives (rssi=-100, snr=0 → metric=50): must NOT replace
     * because 100 → 50 is a regression (50 < 100), not an improvement.    */
    route_cache_learn_rx(&rc2, NODE_A, 0u, 0u, -100, 0, t2);
    re = route_cache_lookup(&rc2, NODE_A, t2);
    CHECK(re != NULL && re->metric == 100u,
          "4g: weaker path (metric=50) does NOT replace strong route (100)");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * main
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    test_stubs_init();

    run1_flood_correctness();
    run2_hybrid_route();
    run3_congestion_and_due_ms();
    run4_link_scoring();

    printf("\n══════════════════════════════════════════\n");
    printf("  PASS: %lu   FAIL: %lu\n",
           (unsigned long)s_pass, (unsigned long)s_fail);
    printf("══════════════════════════════════════════\n");

    return (s_fail > 0u) ? 1 : 0;
}
