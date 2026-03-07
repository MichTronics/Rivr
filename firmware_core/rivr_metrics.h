/* rivr_metrics.h – unified, always-zero-initialised counters.
 * Include wherever you need to increment a counter.
 * Print periodically with rivr_metrics_print().               */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t rx_decode_fail;   /* malformed / foreign frames silently discarded */
    uint32_t rx_dedupe_drop;   /* already-seen (src,seq) discarded              */
    uint32_t rx_ttl_drop;      /* arrived with TTL == 0                         */
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
    uint32_t radio_rx_timeout;    /* RX silent >60 s while in RX → soft reset  */
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
    uint32_t radio_reset_rx_timeout;   /* guard resets triggered by RX-silence timeout   */
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
    uint32_t route_cache_miss_total;          /**< route_cache_lookup returned NULL           */
    uint32_t route_cache_hit_total;           /**< route_cache_lookup returned valid entry    */
} rivr_metrics_t;

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
} rivr_live_stats_t;

/**
 * Emit one @MET JSON line on stdout with all counters plus live stats.
 * @param live  Pointer to live stats struct, or NULL for all-zero live fields.
 */
void rivr_metrics_print(const rivr_live_stats_t *live);
