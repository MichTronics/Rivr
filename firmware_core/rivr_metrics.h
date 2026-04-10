/* rivr_metrics.h – unified, always-zero-initialised counters.
 * Include wherever you need to increment a counter.
 * Print periodically with rivr_metrics_print().               */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t rx_decode_fail;   /* malformed / foreign frames silently discarded */
    uint32_t rx_dedupe_drop;   /* already-seen (src,seq) discarded              */
    uint32_t rx_ttl_drop;      /* arrived with TTL == 0                         */
    uint32_t rx_invalid_type;  /* pkt_type 0 or > PKT_DELIVERY_RECEIPT — unsupported type */
    uint32_t rx_invalid_hop;   /* hop count already >= max TTL (corrupt frame)  */
    uint32_t tx_queue_full;    /* rf_tx_sink_cb could not push to TX ring       */
    uint32_t duty_blocked;     /* dutycycle_check() denied a TX attempt         */
    uint32_t fabric_drop;        /* rivr_fabric scored packet above DROP_THRESHOLD*/
    uint32_t fabric_delay;       /* rivr_fabric deferred a relay (FABRIC_DELAY)   */
    uint32_t radio_busy_stall; /* BUSY stuck before TX; SPI write skipped       */
    uint32_t radio_tx_fail;    /* TX timeout or TX deadline exceeded            */
    uint32_t radio_hard_reset; /* Full SX1262 re-init triggered by fail streak  */
    uint32_t radio_rx_crc_fail;/* SX1262 CRC-error IRQ on received frame        */
    uint32_t pq_dropped;       /* pending_queue full — frame silently dropped   */
    uint32_t pq_expired;       /* pending entries evicted on expiry timeout     */
    uint32_t pq_peak;          /* high-water mark of pending queue occupancy    */
    uint32_t rcache_evict;     /* route cache forced eviction (table full)      */
    uint32_t loop_jitter_ms;   /* max main-loop work duration in ms (gauge)     */
    uint32_t radio_rx_timeout;    /* RX silent >60 s while in RX → metric/log only */
    uint32_t radio_reset_backoff; /* hard reset denied – backoff cooldown active*/
    /* ── Step 4: queue / backpressure drops ─────────────────────────────── */
    uint32_t drop_no_route;      /* pending queue full — originated frame lost  */
    uint32_t drop_rate_limited;  /* RIVR_FWD_DROP_BUDGET: relay rate-limited    */
    uint32_t drop_ttl_relay;     /* RIVR_FWD_DROP_TTL: relay TTL=0 dropped      */
    uint32_t tx_queue_peak;      /* high-water mark of rf_tx_queue occupancy    */
    /* ── Step 6: airtime token-bucket fairness ───────────────────────────── */
    uint32_t airtime_tokens_low; /* times global bucket fell below low-watermark*/
    uint32_t class_drops_ctrl;   /* CONTROL class drops (should always be 0)    */
    uint32_t class_drops_chat;   /* CHAT frames dropped by token gate           */
    uint32_t class_drops_metrics;/* METRICS frames dropped by token gate        */
    uint32_t class_drops_bulk;   /* BULK frames dropped by token gate           */
    /* ── Step 9: SX1262 recovery hardening ──────────────────────────────── */
    uint32_t radio_busy_timeout_total; /* wait_busy() returned false (BUSY pin stuck)   */
    uint32_t tx_timeout_total;         /* SX1262 HW IRQ Timeout flag fired during TX     */
    uint32_t tx_deadline_total;        /* SW poll deadline (toa×2+100ms) exceeded in TX  */
    uint32_t radio_reset_busy_stuck;   /* guard resets triggered by BUSY-stuck streak    */
    uint32_t radio_reset_tx_timeout;   /* guard resets triggered by TX timeout streak    */
    uint32_t radio_reset_spurious_irq; /* guard resets triggered by spurious DIO1 streak */
    uint32_t radio_reset_rx_timeout;   /* reserved compatibility counter; no longer used */
    /* ── P2: signed OTA + policy engine ─────────────────────────────────── */
    uint32_t ota_accepted;       /* signed OTA verifications that passed              */
    uint32_t ota_rejected;       /* OTA rejected: bad sig, replay, or short payload   */
    uint32_t policy_drop;        /* packets dropped by policy token-bucket gate       */
    uint32_t policy_ttl_clamp;   /* packets TTL-clamped by policy engine              */    /* ── Loop-guard loop detection ────────────────────────────────────────── */
    uint32_t loop_detect_drop;   /* RIVR_FWD_DROP_LOOP: relay fingerprint matched     */    /* ── RF airtime accounting (Phase 4 / EU868 compliance audit) ───────── */
    /** Cumulative time-on-air in milliseconds for all transmitted frames.
     *  Incremented in the TX path after each successful radio_transmit() call.
     *  Mirrors dc_ctx_t.used_us but in uint32 ms for the @MET JSON output.
     *  Saturates at UINT32_MAX (~49 days of continuous TX — will never happen). */
    uint32_t rf_airtime_ms_total;
    /** Total TX attempts blocked by the duty-cycle gate (dutycycle_check()
     *  returned false).  Combines the hardware and token-bucket layers.
     *  Distinct from duty_blocked (which is only the dc_ctx hardware gate). */
    uint32_t rf_duty_blocked_total;
    /* ── Routing control-plane observability ─────────────────────────────── */
    uint32_t route_req_rx_total;              /**< ROUTE_REQ frames received                  */
    uint32_t route_req_reply_sent_total;      /**< ROUTE_RPL enqueued in response             */
    uint32_t route_req_reply_cache_total;     /**< replies sourced from route cache           */
    uint32_t route_req_reply_target_total;    /**< replies because we ARE the destination     */
    uint32_t route_req_reply_suppressed_total;/**< ROUTE_REQ heard — no eligible reply        */
    uint32_t route_rpl_rx_total;              /**< ROUTE_RPL frames received                  */
    uint32_t route_rpl_learn_total;           /**< route-cache entries written from RPL       */
    uint32_t pending_queue_drained_total;     /**< frames drained from pending queue          */
    uint32_t pending_queue_expired_total;     /**< pending entries evicted on expiry          */
    uint32_t loop_detect_drop_total;          /**< RIVR_FWD_DROP_LOOP relay drops (alias)     */
    uint32_t forward_drop_ttl_total;          /**< RIVR_FWD_DROP_TTL: relay dropped TTL=0     */
    uint32_t route_cache_miss_total;          /**< route_cache_lookup returned NULL           */
    uint32_t route_cache_hit_total;           /**< route_cache_lookup returned valid entry    */
    /* ── Reliability layer: ACK + retry counters ─────────────────────────── */
    uint32_t ack_tx_total;         /**< PKT_ACK frames sent by this node              */
    uint32_t ack_rx_total;         /**< PKT_ACK frames received                       */
    uint32_t retry_attempt_total;  /**< retry transmissions emitted by retry_table_tick */
    uint32_t retry_success_total;  /**< retry entries cleared by ACK                  */
    uint32_t retry_fail_total;     /**< retry entries exhausted without ACK           */
    uint32_t retry_fallback_total; /**< fallback floods sent after retry exhaustion    */
    uint32_t fallback_flood_total; /**< TX-queue-full fallback floods (rivr_sinks path) */
    /* ── Neighbor-aware next-hop routing ────────────────────────────────── */
    uint32_t neighbor_route_used_total;   /**< unicast used neighbor-quality best-hop      */
    uint32_t neighbor_route_failed_total; /**< best-hop returned score=0 → fell back flood */    /* ── Phase 0: next-gen routing telemetry foundation ──────────────────────── *
     * All four counters are incremented unconditionally (no feature-flag     *
     * guard) so Phase 1–4 baseline comparisons hold before new behaviors    *
     * are enabled.  The last three will remain zero until the corresponding  *
     * phase flag is enabled.                                                 */

    /** Total times RIVR_FWD_FORWARD was returned for flood relay.
     *  Cascaded suppressions (policy, fabric, client gate, opportunistic
     *  cancel) reduce effective relay count below this figure.
     *  Phase 3 uses this as the normalizer for effective relay rate.        */
    uint32_t flood_fwd_attempted_total;

    /** Relays cancelled because a neighbor was heard forwarding the same
     *  (src_id, pkt_id) during our jitter hold-off window.
     *  Zero until RIVR_FEATURE_OPPORTUNISTIC_FWD=1 (Phase 4).              */
    uint32_t flood_fwd_cancelled_opport_total;

    /** Relays suppressed by fwdset quality gate: viable_count > 0 but this
     *  node's best direct score is below FWDSET_MIN_RELAY_SCORE.
     *  Zero until RIVR_FEATURE_OPPORTUNISTIC_FWD=1 (Phase 5).              */
    uint32_t flood_fwd_score_suppressed_total;

    /** Next-hop chosen by airtime-aware ETX scoring (Phase 2 path).
     *  Zero until RIVR_FEATURE_AIRTIME_ROUTING=1.                           */
    uint32_t airtime_route_selected_total;

    /** Airtime scoring had insufficient data → fell back to hop-count.
     *  Zero until RIVR_FEATURE_AIRTIME_ROUTING=1.                           */
    uint32_t airtime_route_fallback_total;

    /* ── Opportunistic relay observability ─────────────────────────────────────
     * These three counters give a complete end-to-end picture of the
     * two-phase opportunistic relay pipeline for every role:
     *
     *   relay_selected (= flood_fwd_attempted_total)
     *     Every time routing_flood_forward() returns RIVR_FWD_FORWARD and
     *     the frame is enqueued for relay.  Includes frames later cancelled
     *     by Phase 4/5.
     *
     *   relay_cancelled (= flood_fwd_cancelled_opport_total)
     *     Relay frames dropped in tx_drain_loop() because a neighbour was
     *     already heard forwarding the same (src_id, pkt_id) during our
     *     jitter hold-off window (Phase 4 reactive suppression).
     *     Note: Phase 5 proactive suppression (score-gate) prevents
     *     enqueue entirely; those are in flood_fwd_score_suppressed_total.
     *
     *   relay_forwarded_total  ← the only genuinely new counter here
     *     Relay frames that completed a successful radio_transmit() (or
     *     sim-TX).  Satisfies:  relay_forwarded ≈ relay_selected
     *       − flood_fwd_score_suppressed_total  (Phase 5, never enqueued)
     *       − flood_fwd_cancelled_opport_total  (Phase 4, cancelled in TX)
     *       − policy/fabric/txqueue drops
     */
    uint32_t relay_forwarded_total; /**< relay frames that completed TX      */

    /** Cumulative extra hold-off applied to relay frames, in milliseconds.
     *  Incremented by fwdset_extra_holdoff_ms() whenever a non-zero holdoff
     *  is added to a relay's due_ms.  Lets operators quantify airtime saved
     *  by adaptive relay delay (density + quality tiers combined).          */
    uint32_t relay_delay_ms_total;

    /* ── Beacon observability (beacon_sched.c + beacon_sink_cb) ─────────────
     * These four counters give operators full visibility into whether the
     * adaptive beacon strategy is working correctly:
     *
     *   beacon_tx_total          — all successful beacon transmissions (TX queued)
     *   beacon_startup_tx_total  — beacons sent during the startup burst phase
     *   beacon_suppressed_total  — beacons skipped by suppression (neighbors
     *                             present or interval/jitter not yet elapsed)
     *   beacon_class_drop        — beacons dropped by the airtime token gate
     *                             (PKT_BEACON now in PKTCLASS_BEACON; this
     *                             should stay near zero with sane intervals)
     */
    uint32_t beacon_tx_total;         /**< All beacon TXs (startup + scheduled)           */
    uint32_t beacon_startup_tx_total; /**< Beacon TXs during startup burst phase           */
    uint32_t beacon_suppressed_total; /**< Beacons suppressed (neighbors or interval gate) */
    uint32_t beacon_class_drop;       /**< Beacons dropped by airtime token gate           */

    /* ── BLE transport observability (firmware_core/ble/) ───────────────────────
     * All four counters are zero when RIVR_FEATURE_BLE=0 (BLE disabled).
     * When BLE is enabled they give operators real-time visibility into
     * the BLE bridge health alongside the LoRa counters.
     *
     *   ble_connections  — cumulative BLE client connections accepted
     *   ble_rx_frames    — frames received from BLE client (injected to mesh)
     *   ble_tx_frames    — frames forwarded to BLE client via TX-notify
     *   ble_errors       — BLE errors: mbuf alloc fail, dropped writes, etc.
     */
    uint32_t ble_connections;  /**< Cumulative BLE client connects              */
    uint32_t ble_rx_frames;    /**< Frames received from BLE client (phone→node) */
    uint32_t ble_tx_frames;    /**< Frames notified to BLE client (node→phone)  */
    uint32_t ble_errors;       /**< BLE stack errors (mbuf alloc fail, drops)   */

    /* ── Packet bus metrics (firmware_core/rivr_bus/) ────────────────────────
     * bus_rx_total       — frames accepted by rivr_bus_receive() (all ifaces)
     * bus_drop_invalid   — frames rejected: length or decode validation fail
     * bus_drop_dup       — frames dropped: cross-transport duplicate cache hit
     * bus_forward_lora   — frames dispatched to LoRa TX queue by bus
     * bus_forward_ble    — frames mirrored to BLE client by bus
     * bus_forward_usb    — frames mirrored to USB bridge by bus
     * bus_errors         — dispatch errors (iface send failures)
     * lora_rx_frames     — LoRa-sourced frames entering the bus
     * usb_rx_frames      — USB-sourced frames entering the bus
     *                      (ble_rx_frames above covers BLE-sourced frames)
     */
    uint32_t bus_rx_total;     /**< Total frames accepted by bus (all ifaces)  */
    uint32_t bus_drop_invalid; /**< Frames dropped: validation failure          */
    uint32_t bus_drop_dup;     /**< Frames dropped: cross-transport dup cache   */
    uint32_t bus_forward_lora; /**< Frames dispatched to LoRa by bus            */
    uint32_t bus_forward_ble;  /**< Frames mirrored to BLE by bus               */
    uint32_t bus_forward_usb;  /**< Frames mirrored to USB by bus               */
    uint32_t bus_errors;       /**< Bus dispatch errors                         */
    uint32_t lora_rx_frames;   /**< LoRa-sourced frames entering bus            */
    uint32_t usb_rx_frames;    /**< USB-sourced frames entering bus             */
    uint32_t rx_net_id_drop;   /**< inbound frames discarded: net_id mismatch   */

    /* ── Private chat counters (firmware_core/private_chat.c) ───────────────
     * All counters zero-initialised (BSS).  Incremented by the private chat
     * engine; read by rivr_metrics_print() and private_chat_print_diag().   */
    uint32_t private_chat_tx_total;              /**< PKT_PRIVATE_CHAT frames sent (originated)  */
    uint32_t private_chat_rx_total;              /**< PKT_PRIVATE_CHAT frames accepted at dest   */
    uint32_t private_chat_receipt_tx_total;      /**< PKT_DELIVERY_RECEIPT frames sent           */
    uint32_t private_chat_receipt_rx_total;      /**< PKT_DELIVERY_RECEIPT frames received       */
    uint32_t private_chat_retry_total;           /**< Private chat frame retransmits             */
    uint32_t private_chat_expired_total;         /**< Outgoing entries expired without delivery  */
    uint32_t private_chat_failed_no_route_total; /**< Expired in AWAITING_ROUTE state            */
    uint32_t private_chat_failed_retry_budget_total; /**< Retry budget exhausted                 */
    uint32_t private_chat_dedup_drop_total;      /**< Duplicate incoming private chat dropped    */
    uint32_t private_chat_invalid_total;         /**< Malformed/invalid private chat frames      */
    uint32_t private_chat_receipt_timeout_total; /**< FORWARDED→UNCONFIRMED receipt timeouts     */
    uint32_t private_chat_queue_depth;           /**< Gauge: current outgoing queue occupancy    */
} rivr_metrics_t;

/* ── BLE metrics payload ────────────────────────────────────────────────────
 * Sent as the body of a PKT_METRICS frame (pkt_type = 11) pushed directly to
 * connected BLE clients every 5 s.  Total frame = 23 (header) + 132 (payload)
 * + 2 (CRC-16) = 157 bytes — fits in one 247-byte-MTU BLE notification.
 *
 * All multi-byte fields are little-endian (native on ESP32 / Xtensa).        */
#define RIVR_MET_BLE_PAYLOAD_LEN  132u

typedef struct __attribute__((packed)) {
    uint32_t node_id;       /**< [0-3]   g_my_node_id                        */
    uint8_t  dc_pct;        /**< [4]     duty-cycle used %                    */
    uint8_t  q_depth;       /**< [5]     TX queue depth                       */
    uint32_t tx_total;      /**< [6-9]   total frames transmitted since boot  */
    uint32_t rx_total;      /**< [10-13] total frames received since boot     */
    uint8_t  route_cache;   /**< [14]    live route-cache entries             */
    uint8_t  lnk_cnt;       /**< [15]    live neighbor count                  */
    uint8_t  lnk_best;      /**< [16]    best neighbor link score 0-100       */
    int8_t   lnk_rssi;      /**< [17]    EWMA RSSI of best neighbor (dBm)     */
    uint8_t  lnk_loss;      /**< [18]    avg packet-loss % across neighbors   */
    uint32_t relay_skip;    /**< [19-22] opportunistic + score suppr. total   */
    uint32_t relay_delay;   /**< [23-26] cumulative relay holdoff ms          */
    uint8_t  relay_density; /**< [27]    viable relay neighbor count          */
    uint32_t relay_fwd;     /**< [28-31] relays that completed TX             */
    uint32_t relay_sel;     /**< [32-35] relay candidates selected            */
    uint32_t relay_can;     /**< [36-39] opportunistic relay cancellations    */
    uint32_t rx_fail;       /**< [40-43] RX frame decode failures             */
    uint32_t rx_dup;        /**< [44-47] RX dedupe drops                      */
    uint32_t rx_ttl;        /**< [48-51] RX TTL drops                         */
    uint32_t rx_bad_type;   /**< [52-55] invalid pkt_type drops               */
    uint32_t rx_bad_hop;    /**< [56-59] invalid hop drops                    */
    uint32_t tx_full;       /**< [60-63] TX queue full drops                  */
    uint32_t dc_blk;        /**< [64-67] duty-cycle blocked sends             */
    uint32_t no_route;      /**< [68-71] no-route drops                       */
    uint32_t loop_drop_total; /**< [72-75] cumulative loop-detect drops       */
    uint32_t rad_rst;       /**< [76-79] radio hard resets                    */
    uint32_t rad_txfail;    /**< [80-83] radio TX failures                    */
    uint32_t rad_crc;       /**< [84-87] radio CRC failures                   */
    uint32_t rc_hit;        /**< [88-91] route-cache hits                     */
    uint32_t rc_miss;       /**< [92-95] route-cache misses                   */
    uint32_t ack_tx;        /**< [96-99] ACK frames transmitted               */
    uint32_t ack_rx;        /**< [100-103] ACK frames received                */
    uint32_t retry_att;     /**< [104-107] retry attempts                     */
    uint32_t retry_ok;      /**< [108-111] successful retries                 */
    uint32_t retry_fail;    /**< [112-115] failed retries                     */
    uint32_t ble_conn;      /**< [116-119] cumulative BLE connections         */
    uint32_t ble_rx;        /**< [120-123] frames received from BLE client    */
    uint32_t ble_tx;        /**< [124-127] frames notified to BLE client      */
    uint32_t ble_err;       /**< [128-131] BLE errors                         */
} rivr_met_ble_payload_t;

_Static_assert(sizeof(rivr_met_ble_payload_t) == RIVR_MET_BLE_PAYLOAD_LEN,
               "rivr_met_ble_payload_t size mismatch");

/**
 * ⚠ THREAD-SAFETY WARNING: g_rivr_metrics is NOT protected by any mutex.
 *
 * All writes MUST originate from the main-loop task (CPU0).
 * Do NOT increment or modify any field from an ISR or a secondary FreeRTOS
 * task — doing so introduces data races on 32-bit counters on Xtensa and can
 * produce silent corruption.  For display / reporting purposes only, reads
 * from the display task are tolerated (torn reads are harmless for gauges).
 */
extern rivr_metrics_t g_rivr_metrics;

/**
 * Live stats populated by the call site and included in the @MET JSON so
 * the Rivr Lab desktop app can render gauges and charts.
 *
 * All fields are optional — pass NULL to omit them (they will be emitted
 * as zero).
 */
typedef struct {
    uint32_t node_id;     /**< Node ID (g_my_node_id)                        */
    uint8_t  dc_pct;      /**< Used duty-cycle %, 0-100                      */
    uint8_t  q_depth;     /**< Current TX queue occupancy (rb_available)     */
    uint32_t tx_total;    /**< Total TX frames since boot (g_tx_frame_count) */
    uint32_t rx_total;    /**< Total RX frames since boot (g_rx_frame_count) */
    uint8_t  route_cache; /**< Live route-cache entries                      */
    /* ── Link quality snapshot (computed from g_ntable at each print) ────────
     * These four fields give a real-time view of neighborhood health.
     * All are zero when no live neighbors are known.                        */
    uint8_t  lnk_cnt;       /**< Count of live (non-stale) neighbors        */
    uint8_t  lnk_best;      /**< Best neighbor_link_score_full (0–100)       */
    int8_t   lnk_best_rssi; /**< EWMA RSSI of best-scoring neighbor (dBm)   */
    uint8_t  lnk_avg_loss;  /**< Average packet-loss % across live neighbors */
    /* ── Adaptive relay density snapshot ────────────────────────────────────
     * viable_count from the most recent fwdset_build() (or lnk_cnt when
     * @MET is emitted outside a relay decision).  Drives the density tier
     * in fwdset_extra_holdoff_ms() and exposed in @MET for observability. */
    uint8_t  relay_density;  /**< Viable neighbor count (score ≥ FWDSET_MIN_RELAY_SCORE) */
} rivr_live_stats_t;

/**
 * Emit one @MET JSON line on stdout with all counters plus live stats.
 * @param live  Pointer to live stats struct, or NULL for all-zero live fields.
 */
void rivr_metrics_print(const rivr_live_stats_t *live);

/**
 * @brief Push a compact binary PKT_METRICS frame to the connected BLE client.
 *
 * Builds a rivr_met_ble_payload_t from @p live and current g_rivr_metrics
 * counters, wraps it in a proper Rivr frame (PKT_METRICS, pkt_type=11),
 * and notifies via rivr_ble_service_notify().  No-op when BLE is not
 * connected or RIVR_FEATURE_BLE=0.
 *
 * Call from the main loop alongside rivr_metrics_print().
 *
 * @param live    Live stats snapshot (same one passed to rivr_metrics_print).
 * @param src_id  This node's node ID (g_my_node_id).
 * @param net_id  Network ID (g_net_id).
 * @param seq     Frame sequence counter (increment before passing).
 */
void rivr_metrics_ble_push(const rivr_live_stats_t *live,
                            uint32_t src_id, uint16_t net_id, uint16_t seq);
