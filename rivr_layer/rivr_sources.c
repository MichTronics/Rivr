/**
 * @file  rivr_sources.c
 * @brief Pull frames from hardware queues and inject into RIVR engine.
 */

#include "rivr_sources.h"
#include "rivr_embed.h"
#include <inttypes.h>   /* PRIu32 */
#include "../firmware_core/platform_esp32.h"  /* UART_CLI_BAUD + pin defs */
#include "../firmware_core/radio_sx1262.h"
#include "../firmware_core/protocol.h"
#include "../firmware_core/timebase.h"
#include "../firmware_core/routing.h"
#include "../firmware_core/route_cache.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "driver/uart.h"

#define TAG "RIVR_SRC"

/* ── Default jitter window ───────────────────────────────────────────────── */
#define FORWARD_JITTER_MAX_TICKS  4u   /**< Jitter window for relay delay     */

/* ── rf_rx source ────────────────────────────────────────────────────────── */

uint32_t sources_rf_rx_drain(void)
{
    uint32_t injected = 0;
    uint32_t now_ms   = tb_millis();

    dedupe_cache_t   *dedupe = routing_get_dedupe();
    forward_budget_t *fb     = routing_get_fwdbudget();

    for (uint32_t i = 0; i < SOURCES_RF_RX_DRAIN_LIMIT; i++) {
        rf_rx_frame_t frame;
        if (!rb_pop(&rf_rx_ringbuf, &frame)) break;  /* ringbuf empty */

        /* ── 1. Validate: must be a RIVR binary protocol packet ── */
        rivr_pkt_hdr_t pkt_hdr;
        const uint8_t *payload_ptr = NULL;
        if (!protocol_decode(frame.data, frame.len, &pkt_hdr, &payload_ptr)) {
            ESP_LOGW(TAG, "rf_rx: invalid frame (len=%u) – dropped", frame.len);
            continue;
        }

        /* ── 2. Phase-A strict flood-forward decision ── *
         * Work on a copy so the original frame bytes are preserved for RIVR. */
        rivr_pkt_hdr_t fwd_hdr = pkt_hdr;
        uint32_t toa_us = RF_TOA_APPROX_US(frame.len);

        rivr_fwd_result_t fwd = routing_flood_forward(dedupe, fb,
                                                       &fwd_hdr, toa_us, now_ms);

        if (fwd == RIVR_FWD_DROP_DEDUPE) {
            /* Seen before — discard completely (no RIVR injection, no learning) */
            ESP_LOGI(TAG, "rf_rx: DEDUPE-DROP src=0x%08lx seq=%lu  [GATE2: (src,seq) keyed, not from_id]",
                     (unsigned long)pkt_hdr.src_id, (unsigned long)pkt_hdr.seq);
            continue;
        }

        /* ── GATE 1 — TTL/hop invariant ────────────────────────────────────── *
         * Every forward must decrement TTL by exactly 1 and increment hop by  *
         * exactly 1, and this mutation must happen BEFORE enqueue.            *
         * routing_flood_forward() is the only site that mutates fwd_hdr, so   *
         * we assert the delta here at the call site immediately after return.  *
         * ─────────────────────────────────────────────────────────────────── */
        if (fwd == RIVR_FWD_FORWARD) {
            uint8_t want_ttl = (uint8_t)(pkt_hdr.ttl - 1u);
            uint8_t want_hop = (uint8_t)(pkt_hdr.hop  + 1u);
            if (fwd_hdr.ttl != want_ttl || fwd_hdr.hop != want_hop) {
                ESP_LOGE(TAG,
                    "GATE1 FAIL src=0x%08lx seq=%lu: "
                    "ttl %u->%u (want %u)  hop %u->%u (want %u)",
                    (unsigned long)pkt_hdr.src_id, (unsigned long)pkt_hdr.seq,
                    pkt_hdr.ttl, fwd_hdr.ttl, want_ttl,
                    pkt_hdr.hop, fwd_hdr.hop, want_hop);
            } else {
                ESP_LOGI(TAG,
                    "GATE1 OK  src=0x%08lx seq=%lu: ttl %u->%u  hop %u->%u",
                    (unsigned long)pkt_hdr.src_id, (unsigned long)pkt_hdr.seq,
                    pkt_hdr.ttl, fwd_hdr.ttl,
                    pkt_hdr.hop, fwd_hdr.hop);
            }
        }

        /* ── 3. Phase-D Reverse-Route Learning ── *
         * Determine immediate sender (from_id):
         *   • frame.from_id != 0  → sim/driver filled it in explicitly
         *   • frame.from_id == 0  → derive from hop count
         *     - hop == 0 → direct; from_id = src_id
         *     - hop >  0 → relay unknown; skip learning
         */
        uint32_t from_id = frame.from_id;
        if (from_id == 0 && pkt_hdr.hop == 0) {
            from_id = pkt_hdr.src_id;   /* direct neighbour */
        }
        route_cache_learn_rx(&g_route_cache,
                              pkt_hdr.src_id, from_id,
                              pkt_hdr.hop,
                              frame.rssi_dbm, now_ms);

        /* ── GATE 3 — Route-cache learning verification ─────────────────────── *
         * Immediately after learning, probe tx_decide so the log shows whether *
         * the route is unicast-ready and which next_hop was chosen.            *
         *   • direct neighbour  → next_hop == src_id                           *
         *   • 2-hop node C via B → next_hop == B, NOT C                        *
         * Also confirms last_seen_ms is refreshed (visible via ttl trace).     *
         * ─────────────────────────────────────────────────────────────────── */
        if (from_id != 0) {
            uint32_t nh = 0;
            rcache_tx_decision_t dec = route_cache_tx_decide(
                &g_route_cache, pkt_hdr.src_id, now_ms, &nh);
            const route_cache_entry_t *re = route_cache_lookup(
                &g_route_cache, pkt_hdr.src_id, now_ms);
            ESP_LOGI(TAG,
                "GATE3 route[0x%08lx]: %s  next_hop=0x%08lx  hops=%u  "
                "metric=%u  last_seen_ms=%lu",
                (unsigned long)pkt_hdr.src_id,
                dec == RCACHE_TX_UNICAST ? "UNICAST" : "FLOOD",
                (unsigned long)nh,
                re ? re->hop_count : 0u,
                re ? re->metric    : 0u,
                re ? (unsigned long)re->last_seen_ms : 0ul);
        }

        /* ── 4. Phase-D: Handle ROUTE_REQ ── */
        if (pkt_hdr.pkt_type == PKT_ROUTE_REQ) {
            /* Are we the requested destination? */
            if (routing_should_reply_route_req(&pkt_hdr, g_my_node_id)) {
                uint8_t rpl_buf[64];
                int rpl_len = routing_build_route_rpl(
                    g_my_node_id,
                    pkt_hdr.src_id,       /* reply to requester */
                    g_my_node_id,         /* target = us */
                    g_my_node_id,         /* next_hop from requester's pov = us */
                    0u,                   /* hop_count = 0 (we ARE target) */
                    ++g_ctrl_seq,
                    rpl_buf, sizeof(rpl_buf));

                if (rpl_len > 0) {
                    rf_tx_request_t rpl_req;
                    memset(&rpl_req, 0, sizeof(rpl_req));
                    memcpy(rpl_req.data, rpl_buf, (uint8_t)rpl_len);
                    rpl_req.len    = (uint8_t)rpl_len;
                    rpl_req.toa_us = RF_TOA_APPROX_US(rpl_req.len);
                    if (rb_try_push(&rf_tx_queue, &rpl_req)) {
                        ESP_LOGI(TAG, "rf_rx: sent ROUTE_RPL to 0x%08lx for target=0x%08lx",
                                 (unsigned long)pkt_hdr.src_id,
                                 (unsigned long)g_my_node_id);
                    }
                }
            }
            /* ROUTE_REQ: handle in C layer, also forward via C relay below,
             * but do NOT inject into RIVR (policy traffic, not application data). */
            goto maybe_relay;
        }

        /* ── 5. Phase-D: Handle ROUTE_RPL ── */
        if (pkt_hdr.pkt_type == PKT_ROUTE_RPL) {
            /* Parse 9-byte payload: target_id (4B) + next_hop (4B) + hop_count (1B) */
            if (payload_ptr && pkt_hdr.payload_len >= ROUTE_RPL_PAYLOAD_LEN) {
                uint32_t target_id = (uint32_t)payload_ptr[0]
                                   | ((uint32_t)payload_ptr[1] <<  8u)
                                   | ((uint32_t)payload_ptr[2] << 16u)
                                   | ((uint32_t)payload_ptr[3] << 24u);
                uint32_t next_hop  = (uint32_t)payload_ptr[4]
                                   | ((uint32_t)payload_ptr[5] <<  8u)
                                   | ((uint32_t)payload_ptr[6] << 16u)
                                   | ((uint32_t)payload_ptr[7] << 24u);
                uint8_t  hops      = payload_ptr[8];

                /* For us: the replier is at pkt_hdr.hop + 1 hops away,
                 * so the full route is: target via replier via from_id. */
                uint8_t metric = (uint8_t)(frame.rssi_dbm < -255 ? 0
                                         : (uint8_t)(255 + frame.rssi_dbm));
                uint32_t effective_next_hop = (from_id != 0) ? from_id : pkt_hdr.src_id;
                route_cache_update(&g_route_cache,
                                   target_id,
                                   effective_next_hop,
                                   (uint8_t)(hops + pkt_hdr.hop + 1u),
                                   metric,
                                   RCACHE_FLAG_VALID,
                                   now_ms);
                ESP_LOGI(TAG, "rf_rx: ROUTE_RPL target=0x%08lx via 0x%08lx hops=%u",
                         (unsigned long)target_id,
                         (unsigned long)next_hop,
                         hops);

                /* ── Phase-D: Drain pending messages for this newly resolved dst ── *
                 * Any frames that were queued in g_pending_queue while we were  *
                 * waiting for this ROUTE_RPL are now re-encoded as unicast TCP  *
                 * hops and pushed directly to rf_tx_queue.                      */
                uint8_t drained = pending_queue_drain_for_dst(
                    &g_pending_queue,
                    target_id, effective_next_hop,
                    &rf_tx_queue, now_ms);
                if (drained > 0u) {
                    ESP_LOGI(TAG,
                        "rf_rx: drained %u pending msg(s) for 0x%08lx via 0x%08lx",
                        drained,
                        (unsigned long)target_id,
                        (unsigned long)effective_next_hop);
                }
            }
            /* ROUTE_RPL: C-layer only, don't inject into RIVR. */
            goto maybe_relay;
        }

        /* ── 6. Inject non-control frames into RIVR ── */
        {
            /* Advance Lamport clock using sender's seq as hint */
            uint32_t assigned_tick = tb_lmp_advance((uint16_t)(pkt_hdr.seq & 0xFFFFu));

            rivr_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.stamp.clock = 1;               /* Clock 1 = Lamport */
            ev.stamp.tick  = assigned_tick;
            ev.v.tag       = RIVR_VAL_BYTES;

            uint16_t copy_len = (frame.len < sizeof(ev.v.as_bytes.buf))
                                ? (uint16_t)frame.len
                                : (uint16_t)sizeof(ev.v.as_bytes.buf);
            memcpy(ev.v.as_bytes.buf, frame.data, copy_len);
            ev.v.as_bytes.len = copy_len;

            rivr_result_t rc = rivr_inject_event("rf_rx", &ev);
            if (rc.code == RIVR_OK) {
                ESP_LOGD(TAG,
                    "rf_rx: injected %u bytes pkt_type=%u src=0x%08lx seq=%lu fwd=%d",
                    copy_len, pkt_hdr.pkt_type,
                    (unsigned long)pkt_hdr.src_id,
                    (unsigned long)pkt_hdr.seq, (int)fwd);
                injected++;
            } else {
                ESP_LOGW(TAG, "rf_rx: inject failed code=%" PRIu32, rc.code);
            }
        }

        /* ── 7. Phase-A relay: re-encode modified header + enqueue with deterministic jitter ── */
        maybe_relay:
        if (fwd == RIVR_FWD_FORWARD) {
            /* routing_forward_delay_ms() gives a deterministic delay in
             * [0..FORWARD_JITTER_MAX_MS] seeded from (src_id, seq, pkt_type).
             * Storing due_ms in the request lets tx_drain_loop() hold the
             * frame until the jitter window expires, avoiding transmit
             * collisions when multiple relays hear the same original packet. */
            uint32_t delay_ms = routing_forward_delay_ms(
                pkt_hdr.src_id, pkt_hdr.seq, pkt_hdr.pkt_type);

            rf_tx_request_t fwd_req;
            memset(&fwd_req, 0, sizeof(fwd_req));
            int enc = protocol_encode(&fwd_hdr, payload_ptr,
                                      fwd_hdr.payload_len,
                                      fwd_req.data, sizeof(fwd_req.data));
            if (enc > 0) {
                fwd_req.len    = (uint8_t)enc;
                fwd_req.toa_us = toa_us;
                fwd_req.due_ms = now_ms + delay_ms;  /* 0 when delay_ms==0 */
                if (rb_try_push(&rf_tx_queue, &fwd_req)) {
                    ESP_LOGD(TAG,
                        "rf_rx: relay queued pkt_type=%u src=0x%08lx ttl=%u delay=%lu ms",
                        fwd_hdr.pkt_type,
                        (unsigned long)fwd_hdr.src_id,
                        fwd_hdr.ttl, (unsigned long)delay_ms);
                } else {
                    ESP_LOGW(TAG, "rf_rx: relay tx_queue full – dropped");
                }
            }
        } else if (fwd == RIVR_FWD_DROP_TTL) {
            ESP_LOGD(TAG, "rf_rx: TTL=0 src=0x%08lx – not relayed",
                     (unsigned long)pkt_hdr.src_id);
        } else if (fwd == RIVR_FWD_DROP_BUDGET) {
            ESP_LOGW(TAG, "rf_rx: fwd budget exceeded pkt_type=%u – not relayed",
                     pkt_hdr.pkt_type);
        }
    }

    return injected;
}

/* ── cli source ──────────────────────────────────────────────────────────── */

/* CLI input line buffer */
static char s_cli_buf[126];
static uint8_t s_cli_pos = 0;

uint32_t sources_cli_drain(void)
{
    /* Read pending chars from UART0 FIFO */
    uint8_t ch;
    while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1) {
        if (ch == '\n' || ch == '\r') {
            if (s_cli_pos == 0) continue;   /* skip empty lines */

            /* Build event: "CLI:<line>" */
            char text_buf[132];
            int n = snprintf(text_buf, sizeof(text_buf), "CLI:");
            memcpy(text_buf + n, s_cli_buf, s_cli_pos);
            text_buf[n + s_cli_pos] = '\0';

            rivr_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.stamp.clock = 0;
            ev.stamp.tick  = tb_millis();
            ev.v.tag       = RIVR_VAL_STR;
            uint8_t len    = (uint8_t)(n + s_cli_pos);
            memcpy(ev.v.as_str.buf, text_buf, len);
            ev.v.as_str.len = len;
            memcpy(ev.kind_tag, "CLI", 4);

            (void)rivr_inject_event("rf_rx", &ev);   /* CLI events go into rf_rx source */
            s_cli_pos = 0;
            return 1;
        } else if (s_cli_pos < sizeof(s_cli_buf) - 1u) {
            s_cli_buf[s_cli_pos++] = (char)ch;
        }
    }
    return 0;
}

/* ── timer source ────────────────────────────────────────────────────────── */

static uint32_t s_last_timer_ms = 0;

uint32_t sources_timer_tick(uint32_t interval_ms)
{
    uint32_t now = tb_millis();
    if (now - s_last_timer_ms < interval_ms) return 0;
    s_last_timer_ms = now;

    char text_buf[32];
    snprintf(text_buf, sizeof(text_buf), "TICK:%lu", (unsigned long)now);

    rivr_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.stamp.clock = 0;
    ev.stamp.tick  = now;
    ev.v.tag       = RIVR_VAL_STR;
    uint8_t len    = (uint8_t)strlen(text_buf);
    memcpy(ev.v.as_str.buf, text_buf, len);
    ev.v.as_str.len = len;
    memcpy(ev.kind_tag, "TICK", 5);

    /* Timer events are injected into a hypothetical "timer" source.
       For now we re-use "rf_rx" to trigger periodic RIVR rules. */
    (void)rivr_inject_event("rf_rx", &ev);
    return 1;
}

/* ── Init ──────────────────────────────────────────────────────────────────── */

void rivr_sources_init(void)
{
    /* UART0 already initialised by IDF as the default console.
       Install UART driver for non-blocking reads if not already done. */
    const uart_config_t uart_cfg = {
        .baud_rate  = UART_CLI_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "rivr_sources_init: done");
}
