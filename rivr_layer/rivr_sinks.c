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
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "RIVR_SINK"

/* ── rf_tx sink ──────────────────────────────────────────────────────────── */

void rf_tx_sink_cb(const rivr_value_t *v, void *user_ctx)
{
    (void)user_ctx;

    if (!v) return;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));

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

    } else if (v->tag == RIVR_VAL_BYTES) {
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
            uint32_t next_hop = 0u;
            rcache_tx_decision_t cache_dec =
                route_cache_tx_decide(&g_route_cache, pkt.dst_id, now_ms, &next_hop);

            if (cache_dec == RCACHE_TX_FLOOD) {
                /* ── Cache MISS for unicast-destined packet ─────────────────
                 * Save to pending queue and send a ROUTE_REQ instead.
                 * The original frame will be drained by the ROUTE_RPL handler
                 * in rivr_sources.c when the route is resolved.             */
                bool pend_ok = pending_queue_enqueue(
                    &g_pending_queue, pkt.dst_id,
                    req.data, req.len, req.toa_us, now_ms);

                /* Build + enqueue ROUTE_REQ broadcast */
                uint8_t rreq_buf[RIVR_PKT_HDR_LEN + RIVR_PKT_CRC_LEN];
                int rreq_enc = routing_build_route_req(
                    g_my_node_id, pkt.dst_id, ++g_ctrl_seq,
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
                ESP_LOGI(TAG,
                    "rf_tx: dst=0x%08lx no route → pending=%s ROUTE_REQ=%s",
                    (unsigned long)pkt.dst_id,
                    pend_ok ? "ok" : "FULL",
                    rreq_ok ? "queued" : "FAIL");
                return;   /* do NOT push original frame; wait for ROUTE_RPL */

            } else {
                /* ── Cache HIT — rewrite as unicast one-hop ──────────────── */
                rivr_pkt_hdr_t uni = pkt;
                uni.dst_id = next_hop;
                uni.ttl    = 1u;
                int reenc = protocol_encode(&uni, pl_copy, pl_len,
                                            req.data, sizeof(req.data));
                if (reenc > 0) {
                    req.len    = (uint8_t)reenc;
                    req.toa_us = RF_TOA_APPROX_US(req.len);
                    ESP_LOGI(TAG,
                        "rf_tx: unicast dst=0x%08lx via next_hop=0x%08lx",
                        (unsigned long)pkt.dst_id, (unsigned long)next_hop);
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
            fb.seq     = ++g_ctrl_seq;               /* fresh seq → no dedupe hit */
            fb.flags   = (uint8_t)((pkt.flags & ~PKT_FLAG_RELAY) | PKT_FLAG_FALLBACK);
            int fb_enc = protocol_encode(&fb, pl_copy, pl_len,
                                         req.data, sizeof(req.data));
            if (fb_enc > 0) {
                req.len    = (uint8_t)fb_enc;
                req.toa_us = RF_TOA_APPROX_US(req.len);
                pushed     = rb_try_push(&rf_tx_queue, &req);
                if (pushed) {
                    ESP_LOGW(TAG,
                        "rf_tx: unicast queue full → FALLBACK flood "
                        "dst=0x%08lx ttl=%u seq=%lu flags=FALLBACK",
                        (unsigned long)pkt.dst_id, RIVR_FALLBACK_TTL,
                        (unsigned long)fb.seq);
                }
            }
        }
        if (!pushed) {
            ESP_LOGW(TAG, "rf_tx: queue full (fallback also failed) – dropped");
        } else {
            ESP_LOGI(TAG, "rf_tx: queued %u bytes (toa=%u us)", req.len, req.toa_us);
            /* Track originated TX separately from relayed traffic so that a
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

    /* ── Non-BYTES path (RIVR_VAL_STR legacy) push ── */
    bool pushed = rb_try_push(&rf_tx_queue, &req);
    if (!pushed) {
        ESP_LOGW(TAG, "rf_tx: queue full – frame dropped (toa=%u us)", req.toa_us);
    } else {
        ESP_LOGI(TAG, "rf_tx: queued %u bytes (toa_approx=%u us, type=0x%02x)",
                 req.len, req.toa_us, req.data[0]);
    }
}

/* ── log sink ────────────────────────────────────────────────────────────── */

void log_sink_cb(const rivr_value_t *v, void *user_ctx)
{
    const char *prefix = user_ctx ? (const char *)user_ctx : "EMIT";

    if (!v) return;

    switch (v->tag) {
        case RIVR_VAL_STR:
            ESP_LOGI("RIVR", "[%s] %.*s", prefix,
                     (int)v->as_str.len, (const char *)v->as_str.buf);
            break;
        case RIVR_VAL_INT:
            ESP_LOGI("RIVR", "[%s] %lld", prefix, (long long)v->as_int);
            break;
        case RIVR_VAL_BOOL:
            ESP_LOGI("RIVR", "[%s] %s", prefix, v->as_bool ? "true" : "false");
            break;
        case RIVR_VAL_BYTES:
            ESP_LOGI("RIVR", "[%s] <bytes len=%u>", prefix, v->as_bytes.len);
            break;
        case RIVR_VAL_UNIT:
        default:
            ESP_LOGI("RIVR", "[%s] ()", prefix);
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

/* ── Init ────────────────────────────────────────────────────────────────── */

void rivr_sinks_init(void)
{
    /* Register rf_tx sink – receives all "rf_tx" emit values */
    rivr_register_sink("rf_tx",      rf_tx_sink_cb,      NULL);

    /* Register usb_print sink – writes plain lines to UART stdout.
       Used by RIVR_SIM_PROGRAM and can be used in hardware builds for
       debug emit { usb_print(x); } statements. */
    rivr_register_sink("usb_print",  usb_print_sink_cb,  NULL);

    /* Register log sink – useful for verbose debug emit { log(x); } */
    rivr_register_sink("log",        log_sink_cb,        (void *)"log");

    ESP_LOGI("RIVR_SINK", "sinks registered: rf_tx, usb_print, log");
}
