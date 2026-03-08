/**
 * @file  routing_stats.h
 * @brief Phase 0 — routing pipeline telemetry snapshot.
 *
 * Single-header implementation: include once from any translation unit that
 * already includes rivr_metrics.h (directly or via rivr_embed.h).  No extra
 * dependencies are introduced.
 *
 * Usage:
 *   uint32_t now_ms = tb_millis();
 *   uint8_t  rc  = route_cache_count(&g_route_cache, now_ms);
 *   uint8_t  nb  = routing_neighbor_count(&g_neighbor_table, now_ms);
 *   uint8_t  pq  = pending_queue_count(&g_pending_queue);
 *   uint8_t  dc  = (uint8_t)( DC_BUDGET_US > 0u
 *                    ? ((DC_BUDGET_US - dutycycle_remaining_us(&g_dc)) * 100ULL
 *                       / DC_BUDGET_US) : 0u );
 *   rivr_routing_stats_t rs = rivr_routing_stats_collect(rc, nb, pq, dc, now_ms);
 *   rivr_routing_stats_print(&rs);
 *
 * Architecture layer mapping:
 *   flood.*        — Phase-A flood-forward pipeline (routing.c / rivr_sources.c)
 *   route.*        — Hybrid unicast route cache (route_cache.c)
 *   nb_routing.*   — Neighbor-quality next-hop selection (routing.c)
 *   reliability.*  — ACK / retry layer (rivr_fabric.c)
 *   next_gen.*     — Phase 0 counters; non-zero only when corresponding
 *                    feature flag (Phase 2–4) is enabled.
 */

#pragma once

#include "rivr_metrics.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Snapshot struct ─────────────────────────────────────────────────────── */

/**
 * Routing pipeline telemetry snapshot — one point-in-time sample.
 *
 * All counters are cumulative since boot; callers that want rate/window
 * figures should compute deltas between two consecutive snapshots.
 *
 * Fields tagged "Phase N placeholder" will remain zero until the
 * corresponding RIVR_FEATURE_* flag is enabled.
 */
typedef struct {

    /* ── Flood pipeline ───────────────────────────────────────────────────── */

    /** RIVR_FWD_FORWARD returned — total relay-eligible packets.
     *  This is the "attempted" total before cascading suppressions apply. */
    uint32_t flood_attempted;

    /** DROP_DEDUPE: exact duplicate seen in de-dupe window.              */
    uint32_t flood_drop_dedupe;

    /** DROP_TTL: arrived at this node with ttl=0 — cannot relay.        */
    uint32_t flood_drop_ttl;

    /** DROP_BUDGET: relay rate-limit (token bucket) hit.                */
    uint32_t flood_drop_budget;

    /** DROP_LOOP: bloom/fingerprint loop-guard hit.                     */
    uint32_t flood_drop_loop;

    /** policy_drop: TTL clamp or duty-cycle policy gate.                */
    uint32_t flood_drop_policy;

    /** fabric_drop: FABRIC_REPEATER congestion gate.                    */
    uint32_t flood_drop_fabric;

    /** tx_queue_full during relay push to RF TX queue.                  */
    uint32_t flood_drop_txq_full;

    /* ── Route cache ─────────────────────────────────────────────────────── */

    uint32_t route_hit;           /**< Unicast cache hit                   */
    uint32_t route_miss;          /**< Cache miss → ROUTE_REQ or flood     */
    uint32_t route_evict;         /**< Cache eviction (table full / expiry) */
    uint8_t  route_live_count;    /**< Live entries at snapshot time       */
    uint8_t  neighbor_live_count; /**< Live neighbor entries               */
    uint8_t  pending_count;       /**< Frames awaiting route resolution    */
    uint8_t  dc_pct;              /**< Duty-cycle used %, 0–100            */

    /* ── Neighbor-quality routing ────────────────────────────────────────── */

    uint32_t nb_route_ok;         /**< Unicast via neighbor-quality hop    */
    uint32_t nb_route_fail;       /**< score=0 → fell back to flood        */

    /* ── Reliability (ACK/retry) ─────────────────────────────────────────── */

    uint32_t ack_rx;
    uint32_t ack_tx;
    uint32_t retry_attempt;
    uint32_t retry_ok;
    uint32_t retry_fail;

    /* ── Phase 0 placeholders ─────────────────────────────────────────────── *
     * Non-zero only when the corresponding RIVR_FEATURE_* flag is active.   */

    /** Phase 2: next-hop chosen by airtime-aware ETX scoring.           */
    uint32_t airtime_route_selected;

    /** Phase 2: ETX scoring fell back to hop-count (insufficient data). */
    uint32_t airtime_route_fallback;

    /** Phase 4: relay cancelled — neighbor heard forwarding same pkt.   */
    uint32_t opport_cancelled;

    /** Phase 5: relay suppressed by fwdset quality gate.                */
    uint32_t score_suppressed;

    /** tb_millis() at the time rivr_routing_stats_collect() was called. */
    uint32_t collected_ms;

} rivr_routing_stats_t;

/* ── Collect ─────────────────────────────────────────────────────────────── */

/**
 * Populate a snapshot from the global g_rivr_metrics counter block.
 *
 * @param rc_live   Live route-cache entry count (route_cache_count()).
 * @param nb_live   Live neighbor entry count (routing_neighbor_count()).
 * @param pending   Pending-queue depth (pending_queue_count()).
 * @param dc_pct    Duty-cycle used %, 0–100 (see usage example at top).
 * @param now_ms    Current tb_millis() timestamp.
 * @return          Populated snapshot by value (32 × uint32_t + 4 × uint8_t).
 */
static inline rivr_routing_stats_t
rivr_routing_stats_collect(uint8_t rc_live, uint8_t nb_live,
                           uint8_t pending, uint8_t dc_pct,
                           uint32_t now_ms)
{
    rivr_routing_stats_t s;

    /* Flood pipeline */
    s.flood_attempted      = g_rivr_metrics.flood_fwd_attempted_total;
    s.flood_drop_dedupe    = g_rivr_metrics.rx_dedupe_drop;
    s.flood_drop_ttl       = g_rivr_metrics.forward_drop_ttl_total;
    s.flood_drop_budget    = g_rivr_metrics.drop_rate_limited;
    s.flood_drop_loop      = g_rivr_metrics.loop_detect_drop_total;
    s.flood_drop_policy    = g_rivr_metrics.policy_drop;
    s.flood_drop_fabric    = g_rivr_metrics.fabric_drop;
    s.flood_drop_txq_full  = g_rivr_metrics.tx_queue_full;

    /* Route cache */
    s.route_hit            = g_rivr_metrics.route_cache_hit_total;
    s.route_miss           = g_rivr_metrics.route_cache_miss_total;
    s.route_evict          = g_rivr_metrics.rcache_evict;
    s.route_live_count     = rc_live;
    s.neighbor_live_count  = nb_live;
    s.pending_count        = pending;
    s.dc_pct               = dc_pct;

    /* Neighbor-quality routing */
    s.nb_route_ok          = g_rivr_metrics.neighbor_route_used_total;
    s.nb_route_fail        = g_rivr_metrics.neighbor_route_failed_total;

    /* Reliability */
    s.ack_rx               = g_rivr_metrics.ack_rx_total;
    s.ack_tx               = g_rivr_metrics.ack_tx_total;
    s.retry_attempt        = g_rivr_metrics.retry_attempt_total;
    s.retry_ok             = g_rivr_metrics.retry_success_total;
    s.retry_fail           = g_rivr_metrics.retry_fail_total;

    /* Phase 0 placeholders */
    s.airtime_route_selected = g_rivr_metrics.airtime_route_selected_total;
    s.airtime_route_fallback = g_rivr_metrics.airtime_route_fallback_total;
    s.opport_cancelled       = g_rivr_metrics.flood_fwd_cancelled_opport_total;
    s.score_suppressed       = g_rivr_metrics.flood_fwd_score_suppressed_total;

    s.collected_ms         = now_ms;
    return s;
}

/* ── Print ───────────────────────────────────────────────────────────────── */

/**
 * Write a human-readable JSON routing-stats block to stdout.
 *
 * Each line is prefixed "@RST " so log parsers / grep can filter it.
 * The JSON is pretty-printed (CRLF) to match the @MET precedent.
 */
static inline void rivr_routing_stats_print(const rivr_routing_stats_t *s)
{
    printf("@RST {\r\n"
           "  \"t_ms\":%" PRIu32 ",\r\n"
           "  \"flood\":{"
             "\"att\":%" PRIu32 ","
             "\"dd\":%" PRIu32 ","
             "\"ttl\":%" PRIu32 ","
             "\"bgt\":%" PRIu32 ","
             "\"loop\":%" PRIu32 ","
             "\"pol\":%" PRIu32 ","
             "\"fab\":%" PRIu32 ","
             "\"txqf\":%" PRIu32
           "},\r\n"
           "  \"route\":{"
             "\"hit\":%" PRIu32 ","
             "\"miss\":%" PRIu32 ","
             "\"evict\":%" PRIu32 ","
             "\"live\":%" PRIu8 ","
             "\"nb_live\":%" PRIu8 ","
             "\"pend\":%" PRIu8 ","
             "\"dc_pct\":%" PRIu8
           "},\r\n"
           "  \"nb\":{"
             "\"ok\":%" PRIu32 ","
             "\"fail\":%" PRIu32
           "},\r\n"
           "  \"rel\":{"
             "\"ack_rx\":%" PRIu32 ","
             "\"ack_tx\":%" PRIu32 ","
             "\"ret_att\":%" PRIu32 ","
             "\"ret_ok\":%" PRIu32 ","
             "\"ret_fail\":%" PRIu32
           "},\r\n"
           "  \"p0\":{"
             "\"at_sel\":%" PRIu32 ","
             "\"at_fb\":%" PRIu32 ","
             "\"opc\":%" PRIu32 ","
             "\"scs\":%" PRIu32
           "}\r\n"
           "}\r\n",
           s->collected_ms,
           /* flood */
           s->flood_attempted,
           s->flood_drop_dedupe,
           s->flood_drop_ttl,
           s->flood_drop_budget,
           s->flood_drop_loop,
           s->flood_drop_policy,
           s->flood_drop_fabric,
           s->flood_drop_txq_full,
           /* route */
           s->route_hit,
           s->route_miss,
           s->route_evict,
           s->route_live_count,
           s->neighbor_live_count,
           s->pending_count,
           s->dc_pct,
           /* nb routing */
           s->nb_route_ok,
           s->nb_route_fail,
           /* reliability */
           s->ack_rx,
           s->ack_tx,
           s->retry_attempt,
           s->retry_ok,
           s->retry_fail,
           /* phase 0 placeholders */
           s->airtime_route_selected,
           s->airtime_route_fallback,
           s->opport_cancelled,
           s->score_suppressed);
}

#ifdef __cplusplus
}
#endif
