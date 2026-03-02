/**
 * @file  rivr_sources.c
 * @brief Pull frames from hardware queues and inject into RIVR engine.
 */

#include "rivr_sources.h"
#include "rivr_embed.h"
#include "rivr_cli.h"
#include <inttypes.h>   /* PRIu32 */
#include "../firmware_core/platform_esp32.h"  /* UART_CLI_BAUD + pin defs */
#include "../firmware_core/radio_sx1262.h"
#include "../firmware_core/protocol.h"
#include "../firmware_core/timebase.h"
#include "../firmware_core/routing.h"
#include "../firmware_core/route_cache.h"
#include "../firmware_core/rivr_fabric.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "driver/uart.h"
#include "../firmware_core/rivr_metrics.h"
#include "../firmware_core/rivr_log.h"
#include "../firmware_core/rivr_ota.h"
#include "../firmware_core/policy.h"
#include "../firmware_core/rivr_policy.h"

#define TAG "RIVR_SRC"

/* ── Default jitter window ───────────────────────────────────────────────── */
#define FORWARD_JITTER_MAX_TICKS  4u   /**< Jitter window for relay delay     */

/* ── Policy engine state ─────────────────────────────────────────────────── */
static policy_state_t g_policy;
static bool g_policy_init_done = false;

/* ── rf_rx source ────────────────────────────────────────────────────────── */

uint32_t sources_rf_rx_drain(void)
{
    if (!g_policy_init_done) {
        policy_init(&g_policy);
        g_policy_init_done = true;
    }

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
            /* Diagnose failure reason so we can distinguish foreign LoRa
             * devices (bad magic) from corrupted RIVR frames (CRC fail). */
            const char *why;
            if (frame.len < RIVR_PKT_MIN_FRAME) {
                why = "too short";
            } else {
                uint16_t m = (uint16_t)(frame.data[0]
                                       | ((uint16_t)frame.data[1] << 8));
                if (m != RIVR_MAGIC) {
                    why = "bad magic (foreign device?)";
                } else {
                    uint8_t pl   = frame.data[21];   /* payload_len byte */
                    uint8_t need = (uint8_t)(RIVR_PKT_HDR_LEN + pl
                                            + RIVR_PKT_CRC_LEN);
                    why = (frame.len < need) ? "length mismatch" : "CRC fail";
                }
            }
            ESP_LOGW(TAG, "rf_rx: invalid frame (len=%u rssi=%d) – %s",
                     frame.len, frame.rssi_dbm, why);
            g_rivr_metrics.rx_decode_fail++;
            continue;
        }
        /* \u2500\u2500 Display stats: count every valid RF reception \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 *
         * Updated before the dedupe check so g_rx_frame_count reflects total  *
         * unique wire receptions, not just non-duplicate ones.                */
        g_rx_frame_count++;
        g_last_rssi_dbm = frame.rssi_dbm;
        g_last_snr_db   = frame.snr_db;
        rivr_fabric_on_rx(now_ms, frame.rssi_dbm, frame.len);
        /* ── 2. Phase-A strict flood-forward decision ── *
         * Work on a copy so the original frame bytes are preserved for RIVR. */
        rivr_pkt_hdr_t fwd_hdr = pkt_hdr;
        uint32_t toa_us = RF_TOA_APPROX_US(frame.len);

        rivr_fwd_result_t fwd = routing_flood_forward(dedupe, fb,
                                                       &fwd_hdr, toa_us, now_ms);

        if (fwd == RIVR_FWD_DROP_DEDUPE) {
            /* Seen before — discard completely (no RIVR injection, no learning) */
            RIVR_LOGI(TAG, "rf_rx: DEDUPE-DROP src=0x%08lx seq=%lu  [GATE2: (src,seq) keyed, not from_id]",
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
                RIVR_LOGI(TAG,
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

        /* ── Display stats: update neighbour table for unique frames ──────────
         * routing_neighbor_update tracks pkt_hdr.src_id + hop_count for all
         * observed senders; hop==0 entries count as direct neighbours.       */
        routing_neighbor_update(&g_neighbor_table, &pkt_hdr,
                                (int8_t)frame.rssi_dbm, frame.snr_db, now_ms);

        route_cache_learn_rx(&g_route_cache,
                              pkt_hdr.src_id, from_id,
                              pkt_hdr.hop,
                              frame.rssi_dbm, frame.snr_db, now_ms);

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
            RIVR_LOGI(TAG,
                "GATE3 route[0x%08lx]: %s  next_hop=0x%08lx  hops=%u  "
                "metric=%u  last_seen_ms=%lu",
                (unsigned long)pkt_hdr.src_id,
                dec == RCACHE_TX_UNICAST ? "UNICAST" : "FLOOD",
                (unsigned long)nh,
                re ? re->hop_count : 0u,
                re ? re->metric    : 0u,
                re ? (unsigned long)re->last_seen_ms : 0ul);
        }

        /* ── 4a. Handle PKT_BEACON: log + learn, skip RIVR injection ────────────────── */
        if (pkt_hdr.pkt_type == PKT_BEACON) {
            char callsign[BEACON_CALLSIGN_MAX + 1] = {0};
            if (payload_ptr && pkt_hdr.payload_len >= BEACON_PAYLOAD_LEN) {
                memcpy(callsign, payload_ptr, BEACON_CALLSIGN_MAX);
                callsign[BEACON_CALLSIGN_MAX] = '\0';
            }
            RIVR_LOGI(TAG, "BEACON src=0x%08lx cs='%s' rssi=%d dBm",
                     (unsigned long)pkt_hdr.src_id, callsign,
                     (int)frame.rssi_dbm);
            /* Persist callsign into the neighbour table entry for this node */
            routing_neighbor_set_callsign(&g_neighbor_table,
                                          pkt_hdr.src_id, callsign);
            goto maybe_relay;
        }

        /* ── 4b. Handle PKT_PROG_PUSH: verify sig + store to NVS + request hot-reload ── */
        if (pkt_hdr.pkt_type == PKT_PROG_PUSH) {
            if (pkt_hdr.dst_id == g_my_node_id || pkt_hdr.dst_id == 0u) {
                if (payload_ptr && pkt_hdr.payload_len > 0u) {
#ifdef RIVR_SIGNED_PROG
                    if (rivr_ota_verify(payload_ptr, pkt_hdr.payload_len)) {
                        if (rivr_ota_activate(payload_ptr, pkt_hdr.payload_len)) {
                            g_program_reload_pending = true;
                            g_rivr_metrics.ota_accepted++;
                            RIVR_LOGI(TAG,
                                "OTA: signed program accepted from 0x%08lx (%u bytes) – reload scheduled",
                                (unsigned long)pkt_hdr.src_id, pkt_hdr.payload_len);
                        }
                    } else {
                        g_rivr_metrics.ota_rejected++;
                        RIVR_LOGW(TAG,
                            "OTA: rejected (bad sig or replay) from 0x%08lx",
                            (unsigned long)pkt_hdr.src_id);
                    }
#else  /* !RIVR_SIGNED_PROG — legacy unsigned path */
                    char prog_buf[2048];
                    /* payload_len is uint8_t (max 255) — always fits in prog_buf[2048] */
                    uint8_t copy_len = pkt_hdr.payload_len;
                    memcpy(prog_buf, payload_ptr, copy_len);
                    prog_buf[copy_len] = '\0';

                    /* ── @PARAMS: policy parameter update (no full program push) ── *
                     * Payload format: "@PARAMS beacon=<ms> chat=<ms> data=<ms> duty=<1..10>"
                     * Example:        "@PARAMS beacon=60000 chat=2000 data=2000 duty=5"
                     * Any omitted key retains its current value.
                     * Values out of bounds are silently ignored (see rivr_policy_set_param).
                     * ─────────────────────────────────────────────────────────────────── */
                    if (copy_len >= 7u &&
                        strncmp(prog_buf, "@PARAMS", 7) == 0) {
                        unsigned long pv = 0;
                        const char *kp;
                        if ((kp = strstr(prog_buf, "beacon=")) != NULL &&
                            sscanf(kp, "beacon=%lu", &pv) == 1)
                            rivr_policy_set_param(RIVR_PARAM_ID_BEACON_INTERVAL, (uint32_t)pv);
                        if ((kp = strstr(prog_buf, "chat=")) != NULL &&
                            sscanf(kp, "chat=%lu", &pv) == 1)
                            rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, (uint32_t)pv);
                        if ((kp = strstr(prog_buf, "data=")) != NULL &&
                            sscanf(kp, "data=%lu", &pv) == 1)
                            rivr_policy_set_param(RIVR_PARAM_ID_DATA_THROTTLE, (uint32_t)pv);
                        if ((kp = strstr(prog_buf, "duty=")) != NULL &&
                            sscanf(kp, "duty=%lu", &pv) == 1)
                            rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, (uint32_t)pv);
                        /* Build updated program from new params and store to NVS   */
                        /* so rivr_embed_reload() picks it up on next iteration.    */
                        char policy_prog[512];
                        rivr_policy_build_program(policy_prog, sizeof(policy_prog));
                        if (rivr_nvs_store_program(policy_prog)) {
                            g_program_reload_pending = true;
                            RIVR_LOGI(TAG,
                                "OTA: @PARAMS update accepted from 0x%08lx – reload scheduled",
                                (unsigned long)pkt_hdr.src_id);
                        }
                    } else {
                        /* Full program text push — existing path */
                        if (rivr_nvs_store_program(prog_buf)) {
                            g_program_reload_pending = true;
                            RIVR_LOGI(TAG,
                                "OTA: new program received from 0x%08lx (%u bytes) – reload scheduled",
                                (unsigned long)pkt_hdr.src_id, copy_len);
                        }
                    }
#endif /* RIVR_SIGNED_PROG */
                }
            } else {
                ESP_LOGD(TAG, "OTA: PKT_PROG_PUSH for 0x%08lx (not us) – discarded",
                         (unsigned long)pkt_hdr.dst_id);
            }
            continue;  /* never relay program pushes */
        }

        /* ── 4c. Phase-D: Handle ROUTE_REQ ── */
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
                        uint32_t _occ = rb_available(&rf_tx_queue);
                        if (_occ > g_rivr_metrics.tx_queue_peak) { g_rivr_metrics.tx_queue_peak = _occ; }
                        RIVR_LOGI(TAG, "rf_rx: sent ROUTE_RPL to 0x%08lx for target=0x%08lx",
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
                RIVR_LOGI(TAG, "rf_rx: ROUTE_RPL target=0x%08lx via 0x%08lx hops=%u",
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
                    RIVR_LOGI(TAG,
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

            /* frame.len is uint8_t (max 255) — always fits in as_bytes.buf[256] */
            uint8_t copy_len = frame.len;
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

        /* ── 6b. Display received chat messages on the client serial console ── */
#if RIVR_ROLE_CLIENT
        if (pkt_hdr.pkt_type == PKT_CHAT
                && payload_ptr != NULL
                && pkt_hdr.payload_len > 0u) {
            rivr_cli_on_chat_rx(pkt_hdr.src_id, payload_ptr, pkt_hdr.payload_len);
        }
#endif /* RIVR_ROLE_CLIENT */

        /* ── 7. Phase-A relay: re-encode modified header + enqueue with deterministic jitter ── */
        maybe_relay:
        if (fwd == RIVR_FWD_FORWARD) {
            /* ── Policy gate: TTL cap + flood token bucket ── */
            {
                uint8_t clamped_ttl = fwd_hdr.ttl;
                rivr_policy_verdict_t pv = policy_check(&g_policy, &fwd_hdr,
                                                        now_ms, &clamped_ttl);
                if (pv == RIVR_POLICY_DROP) {
                    g_rivr_metrics.policy_drop++;
                    ESP_LOGD(TAG,
                        "rf_rx: POLICY drop pkt_type=%u src=0x%08lx",
                        fwd_hdr.pkt_type, (unsigned long)fwd_hdr.src_id);
#if RIVR_ROLE_CLIENT
                    goto skip_enqueue_client;
#elif RIVR_FABRIC_REPEATER
                    goto skip_enqueue;
#else
                    goto skip_relay_policy;
#endif
                }
                if (pv == RIVR_POLICY_TTL_CLAMP) {
                    fwd_hdr.ttl = clamped_ttl;
                    g_rivr_metrics.policy_ttl_clamp++;
                    ESP_LOGD(TAG,
                        "rf_rx: POLICY ttl_clamp pkt_type=%u src=0x%08lx new_ttl=%u",
                        fwd_hdr.pkt_type, (unsigned long)fwd_hdr.src_id, clamped_ttl);
                }
            }
#if RIVR_ROLE_CLIENT
            /* Client nodes do not relay PKT_CHAT or PKT_DATA — those frames
             * originate here or are consumed here, never re-broadcast.
             * Control frames (BEACON, ROUTE_REQ, ROUTE_RPL, ACK, PROG_PUSH)
             * are relayed normally so the mesh routing layer stays intact. */
            if (fwd_hdr.pkt_type == PKT_CHAT || fwd_hdr.pkt_type == PKT_DATA) {
                ESP_LOGD(TAG,
                    "rf_rx: client no-relay pkt_type=%u src=0x%08lx",
                    fwd_hdr.pkt_type, (unsigned long)fwd_hdr.src_id);
                goto skip_enqueue_client;
            }
#endif /* RIVR_ROLE_CLIENT */
            /* routing_forward_delay_ms() gives a deterministic delay in
             * [0..FORWARD_JITTER_MAX_MS] seeded from (src_id, seq, pkt_type).
             * Storing due_ms in the request lets tx_drain_loop() hold the
             * frame until the jitter window expires, avoiding transmit
             * collisions when multiple relays hear the same original packet. */
            uint32_t delay_ms = routing_forward_delay_ms(
                pkt_hdr.src_id, pkt_hdr.seq, pkt_hdr.pkt_type);

            /* ── Fabric relay gate (repeater-only, CHAT/DATA only) ─────────────
             * rivr_fabric_decide_relay() is a no-op (SEND_NOW) unless
             * RIVR_FABRIC_REPEATER=1 was set at compile time AND the
             * packet type is PKT_CHAT or PKT_DATA.  All other types
             * (BEACON, ROUTE_REQ, ROUTE_RPL, ACK, PROG_PUSH) are always
             * SEND_NOW per the guards inside rivr_fabric_decide_relay().   */
#if RIVR_FABRIC_REPEATER
            uint32_t fabric_extra_ms = 0u;
            fabric_decision_t fab_dec = rivr_fabric_decide_relay(
                &fwd_hdr, now_ms, toa_us, &fabric_extra_ms);
            if (fab_dec == FABRIC_DROP) {
                ESP_LOGD(TAG,
                    "rf_rx: FABRIC drop relay pkt_type=%u src=0x%08lx",
                    fwd_hdr.pkt_type, (unsigned long)fwd_hdr.src_id);
                /* skip enqueue — frame relay suppressed by fabric */
                goto skip_enqueue;
            }
            if (fab_dec == FABRIC_DELAY) {
                delay_ms += fabric_extra_ms;
                ESP_LOGD(TAG,
                    "rf_rx: FABRIC delay relay pkt_type=%u src=0x%08lx "
                    "extra=%lu ms",
                    fwd_hdr.pkt_type, (unsigned long)fwd_hdr.src_id,
                    (unsigned long)fabric_extra_ms);
            }
            /* ── Pending-queue backpressure gate ──────────────────────────────
             * When our pending queue is ≥75% full, suppress relay of
             * PKT_CHAT and PKT_DATA to allow originated traffic to drain
             * first.  Pure control packets (BEACON, ROUTE_REQ, ROUTE_RPL,
             * ACK, PROG_PUSH) are always forwarded regardless of pressure. */
            if ((fwd_hdr.pkt_type == PKT_CHAT || fwd_hdr.pkt_type == PKT_DATA)
                    && pending_queue_pressure(&g_pending_queue) >= 75u) {
                ESP_LOGD(TAG,
                    "rf_rx: backpressure relay suppressed pkt_type=%u",
                    fwd_hdr.pkt_type);
                goto skip_enqueue;
            }
#endif /* RIVR_FABRIC_REPEATER */

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
                    uint32_t _occ = rb_available(&rf_tx_queue);
                    if (_occ > g_rivr_metrics.tx_queue_peak) { g_rivr_metrics.tx_queue_peak = _occ; }
                    ESP_LOGD(TAG,
                        "rf_rx: relay queued pkt_type=%u src=0x%08lx ttl=%u delay=%lu ms",
                        fwd_hdr.pkt_type,
                        (unsigned long)fwd_hdr.src_id,
                        fwd_hdr.ttl, (unsigned long)delay_ms);
                } else {
                    g_rivr_metrics.tx_queue_full++;
                    ESP_LOGW(TAG, "rf_rx: relay tx_queue full – dropped");
                }
            }

#if RIVR_FABRIC_REPEATER
            skip_enqueue:;
#endif
#if RIVR_ROLE_CLIENT
            skip_enqueue_client:;
#endif
#if !defined(RIVR_ROLE_CLIENT) && !defined(RIVR_FABRIC_REPEATER)
            /* Used when neither RIVR_ROLE_CLIENT nor RIVR_FABRIC_REPEATER is
             * defined — pure repeater without fabric relay logic. */
            skip_relay_policy:;
#endif
        } else if (fwd == RIVR_FWD_DROP_TTL) {
            g_rivr_metrics.drop_ttl_relay++;
            ESP_LOGD(TAG, "rf_rx: TTL=0 src=0x%08lx – not relayed",
                     (unsigned long)pkt_hdr.src_id);
        } else if (fwd == RIVR_FWD_DROP_BUDGET) {
            g_rivr_metrics.drop_rate_limited++;
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
static bool    s_uart_cli_ready = false;  /**< true only if UART driver installed */

uint32_t sources_cli_drain(void)
{
    if (!s_uart_cli_ready) return 0;  /* no UART driver — non-blocking early exit */

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

typedef struct {
    char     name[32];       /**< Source name (NUL-terminated)               */
    uint32_t interval_ms;    /**< Fire interval in milliseconds               */
    uint32_t last_fire_ms;   /**< Monotonic ms of last fire (or init)        */
    bool     active;         /**< Slot in use                                */
} rivr_timer_entry_t;

static rivr_timer_entry_t s_timers[RIVR_TIMER_MAX];
static uint8_t            s_timer_count = 0u;

void sources_register_timer(const char *name, uint32_t interval_ms)
{
    if (!name || interval_ms == 0u || s_timer_count >= RIVR_TIMER_MAX) return;

    rivr_timer_entry_t *e = &s_timers[s_timer_count++];
    strncpy(e->name, name, sizeof(e->name) - 1u);
    e->name[sizeof(e->name) - 1u] = '\0';
    e->interval_ms  = interval_ms;
    e->last_fire_ms = tb_millis();   /* start counting from registration */
    e->active       = true;
    RIVR_LOGI(TAG, "sources: registered timer '%s' interval=%lu ms",
             e->name, (unsigned long)interval_ms);
}

void sources_timer_reset(void)
{
    memset(s_timers, 0, sizeof(s_timers));
    s_timer_count = 0u;
    RIVR_LOGI(TAG, "sources: timer table cleared");
}

uint32_t sources_timer_drain(void)
{
    uint32_t fired = 0u;
    uint32_t now   = tb_millis();

    for (uint8_t i = 0u; i < s_timer_count; i++) {
        rivr_timer_entry_t *e = &s_timers[i];
        if (!e->active || e->interval_ms == 0u) continue;
        if (now - e->last_fire_ms < e->interval_ms) continue;

        e->last_fire_ms = now;

        rivr_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.stamp.clock = 0;                      /* clock 0 = monotonic */
        ev.stamp.tick  = now;
        ev.v.tag       = RIVR_VAL_INT;
        ev.v.as_int    = (int64_t)now;

        rivr_result_t rc = rivr_inject_event(e->name, &ev);
        if (rc.code == RIVR_OK) {
            fired++;
            ESP_LOGD(TAG, "sources: timer '%s' fired at %lu ms",
                     e->name, (unsigned long)now);
        } else {
            ESP_LOGW(TAG, "sources: timer '%s' inject failed code=%u",
                     e->name, (unsigned)rc.code);
        }
    }
    return fired;
}

/* ── Init ──────────────────────────────────────────────────────────────────── */

void rivr_sources_init(void)
{
#ifdef RIVR_SIM_MODE
    /* UART0 CLI only needed in simulation mode.
     * On real hardware, UART0 is the ESP-IDF console; installing an extra
     * uart driver on top causes rx_mux conflicts that block the main loop. */
    const uart_config_t uart_cfg = {
        .baud_rate  = UART_CLI_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    s_uart_cli_ready = true;
#endif
    RIVR_LOGI(TAG, "rivr_sources_init: done");
}
