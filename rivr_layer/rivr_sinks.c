/**
 * @file  rivr_sinks.c
 * @brief RIVR emit callbacks: encode emitted values back to hardware queues.
 *
 * Phase-D TX path for RIVR_VAL_BYTES (in order):
 *  1. Decode header to inspect dst_id.
 *  2. If dst_id != 0  AND  route_cache MISS:
 *       → enqueue full frame in g_pending_queue
 *       → broadcast PKT_ROUTE_REQ for dst_id
 *       → return without pushing original frame (wait for ROUTE_RPL)
 *  3. If dst_id != 0  AND  route_cache HIT:
 *       → rewrite dst_id = next_hop, ttl = 1 (unicast one hop)
 *  4. rb_try_push to rf_tx_queue.
 *  5. If push fails AND original dest was unicast:
 *       → rebuild as fallback flood: dst=0, ttl=RIVR_FALLBACK_TTL, PKT_FLAG_FALLBACK
 *       → try push again
 */

#include "rivr_sinks.h"
#include "rivr_embed.h"
#include "../firmware_core/radio_sx1262.h"
#include "../firmware_core/timebase.h"
#include "../firmware_core/protocol.h"
#include "../firmware_core/route_cache.h"
#include "../firmware_core/routing.h"
#include "../firmware_core/pending_queue.h"
#include "../firmware_core/beacon_sched.h"   /* beacon scheduling state machine */
#include "../firmware_core/rivr_policy.h"    /* rivr_policy_params_get()        */
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "../firmware_core/rivr_metrics.h"
#include "../firmware_core/rivr_log.h"

#define TAG "RIVR_SINK"

/** Update tx_queue_peak gauge after a successful push to rf_tx_queue. */
#define UPDATE_TXQ_PEAK() do { \
    uint32_t _occ = rb_available(&rf_tx_queue); \
    if (_occ > g_rivr_metrics.tx_queue_peak) { \
        g_rivr_metrics.tx_queue_peak = _occ; \
    } \
} while (0)

/* ── rf_tx sink ──────────────────────────────────────────────────────────── */

void rf_tx_sink_cb(const rivr_value_t *v, void *user_ctx)
{
    (void)user_ctx;

    if (!v) return;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));

#if !RIVR_SIM_MODE
    if (v->tag == RIVR_VAL_STR) {
        /* Legacy: Decode "CHAT:<text>" → frame type 0x04 + Lamport tick header + text */
        const char *s    = (const char *)v->as_str.buf;
        uint8_t     slen = v->as_str.len;

        uint8_t frame_type     = RF_FRAME_DATA;
        uint8_t payload_offset = 0;

        if (slen >= 5u && s[4] == ':') {
            if      (s[0]=='C' && s[1]=='H') { frame_type = RF_FRAME_CHAT;   payload_offset = 5u; }
            else if (s[0]=='B' && s[1]=='E') { frame_type = RF_FRAME_BEACON; payload_offset = 7u; }
            else if (s[0]=='A' && s[1]=='C') { frame_type = RF_FRAME_ACK;    payload_offset = 4u; }
            else                              { frame_type = RF_FRAME_DATA;   payload_offset = 5u; }
        } else if (slen >= 4u && s[3] == ':') {
            if (s[0]=='A' && s[1]=='C' && s[2]=='K') { frame_type = RF_FRAME_ACK; payload_offset = 4u; }
        }

        uint16_t lmp = (uint16_t)tb_lmp_now();
        req.data[0] = frame_type;
        req.data[1] = (uint8_t)(lmp & 0xFFu);
        req.data[2] = (uint8_t)(lmp >> 8u);

        uint8_t plen = (slen >= payload_offset) ? (slen - payload_offset) : 0u;
        if (plen > RF_MAX_PAYLOAD_LEN - 3u) {
            plen = RF_MAX_PAYLOAD_LEN - 3u;
            ESP_LOGW(TAG, "rf_tx: payload truncated to %u bytes", plen);
        }
        memcpy(req.data + 3u, s + payload_offset, plen);
        req.len    = 3u + plen;
        req.toa_us = RF_TOA_APPROX_US(req.len);

    } else
#endif /* !RIVR_SIM_MODE */
    if (v->tag == RIVR_VAL_BYTES) {
        /* ── Phase-D unicast path ─────────────────────────────────────────── */
        uint16_t total = v->as_bytes.len;
        if (total > RF_MAX_PAYLOAD_LEN) {
            total = RF_MAX_PAYLOAD_LEN;
            ESP_LOGW(TAG, "rf_tx: raw bytes truncated to %u", RF_MAX_PAYLOAD_LEN);
        }
        memcpy(req.data, v->as_bytes.buf, total);
        req.len    = (uint8_t)total;
        req.toa_us = RF_TOA_APPROX_US(req.len);

        /* Decode header to inspect dst_id */
        rivr_pkt_hdr_t pkt;
        const uint8_t *pl  = NULL;
        bool           dec = protocol_decode(req.data, req.len, &pkt, &pl);
        uint32_t       now_ms = tb_millis();

        /* Save payload bytes before any re-encoding so the fallback path
         * can rebuild from the original (pl pointer is into req.data which
         * may be overwritten by re-encode). */
        uint8_t pl_copy[RIVR_PKT_MAX_PAYLOAD];
        uint8_t pl_len = 0u;
        if (dec && pl && pkt.payload_len > 0u) {
            pl_len = pkt.payload_len;
            if (pl_len > RIVR_PKT_MAX_PAYLOAD) pl_len = RIVR_PKT_MAX_PAYLOAD;
            memcpy(pl_copy, pl, pl_len);
        }

        if (dec && pkt.dst_id != 0u) {
            rivr_route_t  chosen;
            bool          have_route =
                route_cache_best_hop(&g_route_cache, &g_ntable,
                                     pkt.dst_id, now_ms, &chosen);

            if (!have_route) {
                /* ── Tier 3: cache MISS + no qualified neighbor forwarder ──
                 * Save to pending queue and send a ROUTE_REQ broadcast.
                 * The original frame will be drained by the ROUTE_RPL handler
                 * in rivr_sources.c when the route is resolved.             */
                bool pend_ok = pending_queue_enqueue(
                    &g_pending_queue, pkt.dst_id,
                    req.data, req.len, req.toa_us, now_ms);

                /* Build + enqueue ROUTE_REQ broadcast */
                ++g_ctrl_seq;
                uint8_t rreq_buf[RIVR_PKT_HDR_LEN + RIVR_PKT_CRC_LEN];
                int rreq_enc = routing_build_route_req(
                    g_my_node_id, pkt.dst_id,
                    (uint16_t)g_ctrl_seq, (uint16_t)g_ctrl_seq,
                    rreq_buf, sizeof(rreq_buf));
                bool rreq_ok = false;
                if (rreq_enc > 0) {
                    rf_tx_request_t rreq;
                    memset(&rreq, 0, sizeof(rreq));
                    memcpy(rreq.data, rreq_buf, (uint8_t)rreq_enc);
                    rreq.len    = (uint8_t)rreq_enc;
                    rreq.toa_us = RF_TOA_APPROX_US(rreq.len);
                    rreq_ok = rb_try_push(&rf_tx_queue, &rreq);
                }
                RIVR_LOGI(TAG,
                    "[ROUTE_REQ] dst=0x%08lx pend=%s sent=%s",
                    (unsigned long)pkt.dst_id,
                    pend_ok ? "ok" : "full",
                    rreq_ok ? "ok" : "fail");
                if (!pend_ok) {
                    g_rivr_metrics.drop_no_route++;
                }
                if (rreq_ok) { UPDATE_TXQ_PEAK(); }
                return;   /* do NOT push original frame; wait for ROUTE_RPL */

            } else {
                /* ── Tier 1 or 2: rewrite as unicast one-hop ──────────────
                 * chosen.next_hop_id comes from either:
                 *   Tier 1 — route-cache hit (best composite-scored entry)
                 *   Tier 2 — best direct neighbor as forwarder             */
                uint32_t next_hop = chosen.next_hop_id;
                rivr_pkt_hdr_t uni = pkt;
                uni.dst_id = next_hop;
                uni.ttl    = 1u;
                int reenc = protocol_encode(&uni, pl_copy, pl_len,
                                            req.data, sizeof(req.data));
                if (reenc > 0) {
                    req.len    = (uint8_t)reenc;
                    req.toa_us = RF_TOA_APPROX_US(req.len);
                    /* For data-plane unicast frames: set ACK_REQ flag in the
                     * wire byte, recompute CRC, then store in the retry table
                     * so we can re-send if no ACK arrives within the timeout.
                     * Control and beacon types never need ACK protection. */
                    if (pkt.pkt_type != PKT_ACK       &&
                        pkt.pkt_type != PKT_ROUTE_REQ &&
                        pkt.pkt_type != PKT_ROUTE_RPL &&
                        pkt.pkt_type != PKT_BEACON) {
                        req.data[4] |= PKT_FLAG_ACK_REQ;
                        uint8_t  _plen = req.data[21];
                        uint8_t  _coff = (uint8_t)(RIVR_PKT_HDR_LEN + _plen);
                        uint16_t _crc  = protocol_crc16(req.data, _coff);
                        req.data[_coff]      = (uint8_t)(_crc & 0xFFu);
                        req.data[_coff + 1u] = (uint8_t)(_crc >> 8u);
                        retry_table_enqueue(&g_retry_table, g_my_node_id,
                                            pkt.pkt_id,
                                            pkt.dst_id, next_hop,
                                            req.data, req.len,
                                            req.toa_us, now_ms);
                    }
                    RIVR_LOGI(TAG,
                        "rf_tx: unicast dst=0x%08lx via next_hop=0x%08lx score=%u hops=%u",
                        (unsigned long)pkt.dst_id, (unsigned long)next_hop,
                        (unsigned)chosen.metric, (unsigned)chosen.hop_count);
                }
            }
        }
        /* dst_id == 0: broadcast flood — leave req unchanged */

        /* ── Push to TX queue ──────────────────────────────────────────────
         * If this was a unicast attempt and the queue is full, fall back to
         * a limited-TTL broadcast so the message is not silently dropped.   */
        bool pushed = rb_try_push(&rf_tx_queue, &req);
        if (!pushed && dec && pkt.dst_id != 0u) {
            /* ── Unicast failover: fallback flood ───────────────────────── *
             * Bump seq so that nodes which already saw the unicast attempt  *
             * (or this node on a retry) do NOT dedupe-drop the flood.       *
             * Set hop=0: this is an originated fallback, not a relay hop.   */
            rivr_pkt_hdr_t fb = pkt;
            fb.dst_id  = 0u;                        /* broadcast */
            fb.ttl     = RIVR_FALLBACK_TTL;          /* limited range */
            fb.hop     = 0u;                         /* originated, not relayed */
            fb.seq     = pkt.seq;                    /* preserve app seq (same logical message) */
            fb.pkt_id  = (uint16_t)++g_ctrl_seq;    /* fresh pkt_id → bypasses dedupe at nodes
                                                      * that already saw the unicast attempt */
            fb.flags   = (uint8_t)((pkt.flags & ~PKT_FLAG_RELAY) | PKT_FLAG_FALLBACK);
            int fb_enc = protocol_encode(&fb, pl_copy, pl_len,
                                         req.data, sizeof(req.data));
            if (fb_enc > 0) {
                req.len    = (uint8_t)fb_enc;
                req.toa_us = RF_TOA_APPROX_US(req.len);
                pushed     = rb_try_push(&rf_tx_queue, &req);
                if (pushed) {
                    g_rivr_metrics.fallback_flood_total++;
                    RIVR_LOGW(TAG,
                        "[FLOOD_FALLBACK] pkt_id=0x%04x dst=0x%08lx reason=queue_full",
                        (unsigned)fb.pkt_id, (unsigned long)pkt.dst_id);
                }
            }
        }
        if (!pushed) {
            g_rivr_metrics.tx_queue_full++;
            ESP_LOGW(TAG, "rf_tx: queue full (fallback also failed) – dropped");
        } else {
            RIVR_LOGI(TAG, "rf_tx: queued %u bytes (toa=%u us)", req.len, req.toa_us);
            UPDATE_TXQ_PEAK();
            g_tx_frame_count++;            /* Track originated TX separately from relayed traffic so that a
             * heavy relay storm cannot exhaust the forward budget and silence
             * this node's own transmissions.  (Check #5 — fwd vs originated) */
            if (dec && pkt.pkt_type < FWDBUDGET_PKT_TYPES) {
                routing_get_fwdbudget()->tx_originated_count[pkt.pkt_type] += 1u;
            }
        }
        return;   /* RIVR_VAL_BYTES path done */

    } else {
        ESP_LOGW(TAG, "rf_tx: unsupported value tag %d – dropped", v->tag);
        return;
    }

#if !RIVR_SIM_MODE
    /* ── Non-BYTES path (RIVR_VAL_STR legacy) push ── */
    bool pushed = rb_try_push(&rf_tx_queue, &req);
    if (!pushed) {
        g_rivr_metrics.tx_queue_full++;
        ESP_LOGW(TAG, "rf_tx: queue full – frame dropped (toa=%u us)", req.toa_us);
    } else {
        RIVR_LOGI(TAG, "rf_tx: queued %u bytes (toa_approx=%u us, type=0x%02x)",
                 req.len, req.toa_us, req.data[0]);
        UPDATE_TXQ_PEAK();
        g_tx_frame_count++;
    }
#endif /* !RIVR_SIM_MODE */
}

/* ── log sink ────────────────────────────────────────────────────────────── */

void log_sink_cb(const rivr_value_t *v, void *user_ctx)
{
    const char *prefix = user_ctx ? (const char *)user_ctx : "EMIT";

    if (!v) return;

    switch (v->tag) {
        case RIVR_VAL_STR:
            RIVR_LOGI("RIVR", "[%s] %.*s", prefix,
                     (int)v->as_str.len, (const char *)v->as_str.buf);
            break;
        case RIVR_VAL_INT:
            RIVR_LOGI("RIVR", "[%s] %lld", prefix, (long long)v->as_int);
            break;
        case RIVR_VAL_BOOL:
            RIVR_LOGI("RIVR", "[%s] %s", prefix, v->as_bool ? "true" : "false");
            break;
        case RIVR_VAL_BYTES:
            RIVR_LOGI("RIVR", "[%s] <bytes len=%u>", prefix, v->as_bytes.len);
            break;
        case RIVR_VAL_UNIT:
        default:
            RIVR_LOGI("RIVR", "[%s] ()", prefix);
            break;
    }
}

/* ── usb_print sink ──────────────────────────────────────────────────────── */

void usb_print_sink_cb(const rivr_value_t *v, void *user_ctx)
{
    (void)user_ctx;
    if (!v) return;

    /* Plain printf so the line appears instantly, without any IDF log framing.
       This is the most readable output for the proof-of-life test. */
    switch (v->tag) {
        case RIVR_VAL_STR:
            /* Null-terminate in a local copy (as_str.buf may not be NUL at [len]) */
            {
                char tmp[129];
                uint8_t n = v->as_str.len < 128u ? v->as_str.len : 128u;
                memcpy(tmp, v->as_str.buf, n);
                tmp[n] = '\0';
                printf("[RIVR-TX] %s\n", tmp);
            }
            break;
        case RIVR_VAL_INT:
            printf("[RIVR-TX] %lld\n", (long long)v->as_int);
            break;
        case RIVR_VAL_BOOL:
            printf("[RIVR-TX] %s\n", v->as_bool ? "true" : "false");
            break;
        case RIVR_VAL_BYTES:
            printf("[RIVR-TX] <bytes len=%u>\n", (unsigned)v->as_bytes.len);
            break;
        case RIVR_VAL_UNIT:
        default:
            printf("[RIVR-TX] ()\n");
            break;
    }
    fflush(stdout);
}

/* ── beacon sink ─────────────────────────────────────────────────────────────── */

/** Beacon scheduling state — BSS, no heap. */
static beacon_sched_t s_beacon_sched = {0};

/**
 * Build and queue one PKT_BEACON frame.
 *
 * Called by both the boot-time immediate beacon (rivr_sinks_init) and by
 * beacon_sink_cb() after the scheduler approves a TX.  Callers are
 * responsible for incrementing beacon_tx_total / beacon_startup_tx_total.
 */
static void do_beacon_tx(void)
{
    /* Build PKT_BEACON payload: callsign[10] + hop_count[1] */
    uint8_t payload[BEACON_PAYLOAD_LEN];
    memset(payload, 0, sizeof(payload));
    size_t cs_len = strlen(g_callsign);
    if (cs_len > BEACON_CALLSIGN_MAX) cs_len = BEACON_CALLSIGN_MAX;
    memcpy(payload, g_callsign, cs_len);
    payload[BEACON_CALLSIGN_MAX] = 0u;   /* hop_count = 0 (origin) */

    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_BEACON;
    hdr.flags       = 0;
    hdr.ttl         = RIVR_PKT_DEFAULT_TTL;
    hdr.hop         = 0;
    hdr.net_id      = g_net_id;
    hdr.src_id      = g_my_node_id;
    hdr.dst_id      = 0;   /* broadcast */
    hdr.seq         = (uint16_t)++g_ctrl_seq;
    hdr.pkt_id      = (uint16_t)g_ctrl_seq;
    hdr.payload_len = BEACON_PAYLOAD_LEN;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    int enc = protocol_encode(&hdr, payload, BEACON_PAYLOAD_LEN,
                               req.data, sizeof(req.data));
    if (enc <= 0) {
        ESP_LOGW(TAG, "beacon: encode failed");
        return;
    }
    req.len    = (uint8_t)enc;
    req.toa_us = RF_TOA_APPROX_US(req.len);
    req.due_ms = 0;

    if (rb_try_push(&rf_tx_queue, &req)) {
        RIVR_LOGI(TAG, "beacon queued: src=0x%08lx cs='%s'",
                 (unsigned long)g_my_node_id, g_callsign);
        g_tx_frame_count++;
    } else {
        ESP_LOGW(TAG, "beacon: tx queue full");
    }
}

void beacon_sink_cb(const rivr_value_t *v, void *user_ctx)
{
    (void)v;         /* beacon fires on any event; value is irrelevant */
    (void)user_ctx;

    uint32_t now_ms = tb_millis();
    const rivr_policy_params_t *p = rivr_policy_params_get();

    /* Compute live neighbor count for adaptive suppression.
     * neighbor_table_link_summary() is an O(16) inline; negligible overhead. */
    neighbor_link_summary_t lnk = neighbor_table_link_summary(&g_ntable, now_ms);

    /* Mix node ID, sequence counter, and time for per-TX entropy.
     * node_id contributes a stable per-node bias so different nodes get
     * different offsets and do not all fire simultaneously after a shared
     * power cycle.  seq and now_ms add per-call variation. */
    uint32_t entropy = g_my_node_id ^ (uint32_t)g_ctrl_seq ^ now_ms;

    beacon_sched_result_t result = beacon_sched_tick(
        &s_beacon_sched,
        now_ms,
        p->beacon_interval_ms,
        p->beacon_jitter_ms,
        lnk.count,
        entropy);

    switch (result) {
        case BEACON_TX_STARTUP:
            do_beacon_tx();
            g_rivr_metrics.beacon_tx_total++;
            g_rivr_metrics.beacon_startup_tx_total++;
            RIVR_LOGI(TAG, "beacon TX (startup burst #%u of %u)",
                     (unsigned)s_beacon_sched.startup_count,
                     (unsigned)BEACON_STARTUP_COUNT);
            break;

        case BEACON_TX_SCHEDULED:
            do_beacon_tx();
            g_rivr_metrics.beacon_tx_total++;
            RIVR_LOGI(TAG, "beacon TX (keepalive) interval=%lums next_min=%lums",
                     (unsigned long)p->beacon_interval_ms,
                     (unsigned long)s_beacon_sched.next_min_tx_ms);
            break;

        case BEACON_SUPPRESS_NEIGHBORS:
            g_rivr_metrics.beacon_suppressed_total++;
            RIVR_LOGD(TAG, "beacon suppressed: live_neighbors=%u >= threshold %u",
                     (unsigned)lnk.count, (unsigned)BEACON_SUPPRESS_MIN_NEIGHBORS);
            break;

        case BEACON_SUPPRESS_INTERVAL:
            g_rivr_metrics.beacon_suppressed_total++;
            RIVR_LOGD(TAG, "beacon suppressed: interval/jitter not elapsed");
            break;
    }
}

/* ── Init ──────────────────────────────────────────────────────────────────── */

void rivr_sinks_init(void)
{
    /* Register rf_tx sink – receives all "rf_tx" emit values */
    rivr_register_sink("io.lora.tx",     rf_tx_sink_cb,   NULL);

    /* Periodic beacon: fires on any event injected to io.lora.beacon */
    rivr_register_sink("io.lora.beacon", beacon_sink_cb,  NULL);

    /* Register usb_print sink */
    rivr_register_sink("io.usb.print",   usb_print_sink_cb, NULL);

    /* Register log sink */
    rivr_register_sink("log",            log_sink_cb,     (void *)"log");

    RIVR_LOGI("RIVR_SINK", "sinks registered: rf_tx, beacon, usb_print, log");
    /* ── Immediate startup beacon ─────────────────────────────────────────
     * Announce presence once at boot before any timer fires.  This seeds     *
     * the scheduler's last_tx_ms so the first periodic timer check has a     *
     * valid reference point, and ensures neighboring nodes learn about us    *
     * even if the long-interval timer has not yet fired.                     */
    {
        uint32_t now_ms = tb_millis();
        beacon_sched_set_initial_tx(&s_beacon_sched, now_ms);
        do_beacon_tx();
        g_rivr_metrics.beacon_tx_total++;
        g_rivr_metrics.beacon_startup_tx_total++;
        RIVR_LOGI("RIVR_SINK", "beacon TX (boot, immediate)");
    }
}
