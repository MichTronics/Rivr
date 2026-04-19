/**
 * @file  rivr_routing_wasm.c
 * @brief Emscripten WASM exports — thin wrapper around the real firmware_core
 *        routing kernel.  The same C sources used by tests/ are compiled here;
 *        no ESP-IDF, no FreeRTOS, no radio driver needed.
 *
 * Exposed JS API (via EMSCRIPTEN_KEEPALIVE / cwrap):
 *
 *   rivr_wasm_init()
 *     Reset all module state.  Call once, or call again to start a new sim.
 *
 *   rivr_wasm_node_init(node_idx)
 *     Initialise per-node dedupe + budget state (up to WASM_MAX_NODES).
 *
 *   rivr_wasm_flood_forward(node_idx, src_id, dst_id, pkt_id, seq,
 *                           ttl, hop, pkt_type, now_ms)
 *     Run routing_flood_forward() for the given node.
 *     Returns one of:
 *       0  RIVR_FWD_FORWARD
 *       1  RIVR_FWD_DROP_DEDUPE
 *       2  RIVR_FWD_DROP_TTL
 *       3  RIVR_FWD_DROP_BUDGET
 *       4  RIVR_FWD_DROP_LOOP
 *
 *   rivr_wasm_get_ttl()   / rivr_wasm_get_hop()
 *     Read back the (mutated) TTL / hop from the last call.
 *
 *   rivr_wasm_get_drop_counts(node_idx, out_dedupe, out_ttl, out_budget)
 *     Fill three uint32_t pointers with per-node metric counters.
 *
 *   rivr_wasm_jitter_ms(pkt_id)
 *     Return the deterministic jitter delay (ms) for this packet id.
 *
 *   rivr_wasm_protocol_version()
 *     Returns RIVR_PROTO_VER (1).
 *
 *   rivr_wasm_default_ttl()
 *     Returns RIVR_PKT_DEFAULT_TTL (currently 15).
 */

#include <stdint.h>
#include <string.h>
#include <emscripten.h>

/* Pull in the real firmware headers */
#include "routing.h"
#include "protocol.h"
#include "rivr_metrics.h"

/* ── Simulation sizing ───────────────────────────────────────────────────── */
#define WASM_MAX_NODES  16u

/* ── Per-node state ──────────────────────────────────────────────────────── */
typedef struct {
    dedupe_cache_t   dedupe;
    forward_budget_t budget;
    uint32_t         node_id;
    /* Metric counters (subset) */
    uint32_t         cnt_dedupe_drop;
    uint32_t         cnt_ttl_drop;
    uint32_t         cnt_budget_drop;
    uint32_t         cnt_forward;
} wasm_node_t;

static wasm_node_t g_nodes[WASM_MAX_NODES];
static uint8_t     g_node_count = 0u;

/* Scratch packet — written by flood_forward, readable via get_ttl/get_hop */
static rivr_pkt_hdr_t g_last_pkt;

/* Global metrics — defined in rivr_metrics.c (already compiled into WASM) */
extern rivr_metrics_t g_rivr_metrics;

/* ── Init ────────────────────────────────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
void rivr_wasm_init(void)
{
    memset(g_nodes,        0, sizeof(g_nodes));
    memset(&g_last_pkt,    0, sizeof(g_last_pkt));
    memset(&g_rivr_metrics,0, sizeof(g_rivr_metrics));
    g_node_count = 0u;
}

EMSCRIPTEN_KEEPALIVE
void rivr_wasm_node_init(uint8_t node_idx, uint32_t node_id)
{
    if (node_idx >= WASM_MAX_NODES) return;
    wasm_node_t *n = &g_nodes[node_idx];
    routing_dedupe_init(&n->dedupe);
    routing_fwdbudget_init(&n->budget);
    n->node_id         = node_id;
    n->cnt_dedupe_drop = 0u;
    n->cnt_ttl_drop    = 0u;
    n->cnt_budget_drop = 0u;
    n->cnt_forward     = 0u;
    if (node_idx >= g_node_count) g_node_count = node_idx + 1u;
}

/* ── Core routing call ───────────────────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
int rivr_wasm_flood_forward(
    uint8_t  node_idx,
    uint32_t src_id,
    uint32_t dst_id,
    uint16_t pkt_id,
    uint16_t seq,
    uint8_t  ttl,
    uint8_t  hop,
    uint8_t  pkt_type,
    uint32_t now_ms)
{
    if (node_idx >= WASM_MAX_NODES) return (int)RIVR_FWD_DROP_BUDGET;

    wasm_node_t *n = &g_nodes[node_idx];

    rivr_pkt_hdr_t pkt = {
        .magic      = RIVR_MAGIC,
        .version    = RIVR_PROTO_VER,
        .pkt_type   = pkt_type,
        .flags      = 0u,
        .ttl        = ttl,
        .hop        = hop,
        .net_id     = 0u,
        .src_id     = src_id,
        .dst_id     = dst_id,
        .seq        = seq,
        .pkt_id     = pkt_id,
        .payload_len= 0u,
        .loop_guard = 0u,
    };

    /* Approximate time-on-air for a small LoRa frame (~20 byte payload, SF9) */
    uint32_t toa_us = routing_toa_estimate_us(20u);

    rivr_fwd_result_t result = routing_flood_forward(
        &n->dedupe, &n->budget, &pkt, n->node_id, toa_us, now_ms);

    /* Save mutated packet for get_ttl / get_hop */
    g_last_pkt = pkt;

    /* Update per-node counters */
    switch (result) {
        case RIVR_FWD_FORWARD:       n->cnt_forward++;      break;
        case RIVR_FWD_DROP_DEDUPE:   n->cnt_dedupe_drop++;  break;
        case RIVR_FWD_DROP_TTL:      n->cnt_ttl_drop++;     break;
        case RIVR_FWD_DROP_BUDGET:   n->cnt_budget_drop++;  break;
        default: break;
    }

    return (int)result;
}

/* ── Accessors for last packet state ─────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE uint8_t  rivr_wasm_get_ttl(void)  { return g_last_pkt.ttl; }
EMSCRIPTEN_KEEPALIVE uint8_t  rivr_wasm_get_hop(void)  { return g_last_pkt.hop; }
EMSCRIPTEN_KEEPALIVE uint8_t  rivr_wasm_get_flags(void){ return g_last_pkt.flags; }

/* ── Per-node metric counters ─────────────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
void rivr_wasm_get_drop_counts(
    uint8_t   node_idx,
    uint32_t *out_dedupe,
    uint32_t *out_ttl,
    uint32_t *out_budget,
    uint32_t *out_forward)
{
    if (node_idx >= WASM_MAX_NODES) return;
    wasm_node_t *n = &g_nodes[node_idx];
    if (out_dedupe)  *out_dedupe  = n->cnt_dedupe_drop;
    if (out_ttl)     *out_ttl     = n->cnt_ttl_drop;
    if (out_budget)  *out_budget  = n->cnt_budget_drop;
    if (out_forward) *out_forward = n->cnt_forward;
}

/* ── Jitter helper ───────────────────────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
uint32_t rivr_wasm_jitter_ms(uint16_t pkt_id)
{
    return routing_jitter_ticks(0u, pkt_id, 8u);
}

/* ── Protocol constants ───────────────────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE uint8_t rivr_wasm_protocol_version(void) { return RIVR_PROTO_VER; }
EMSCRIPTEN_KEEPALIVE uint8_t rivr_wasm_default_ttl(void)      { return RIVR_PKT_DEFAULT_TTL; }
EMSCRIPTEN_KEEPALIVE uint8_t rivr_wasm_max_nodes(void)        { return WASM_MAX_NODES; }
