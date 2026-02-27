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
    uint32_t radio_busy_timeout; /* BUSY pin stuck before TX; frame skipped       */
    uint32_t radio_tx_fail;      /* TX timeout or TX deadline exceeded            */
    uint32_t radio_hard_reset;   /* Full SX1262 re-init triggered by fail streak  */
} rivr_metrics_t;

extern rivr_metrics_t g_rivr_metrics;

/** Emit one log line (ESP_LOGI) with all counters. */
void rivr_metrics_print(void);
