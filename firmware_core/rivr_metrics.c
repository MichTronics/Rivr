#include "rivr_metrics.h"
#include "rivr_log.h"
#include <stdio.h>
#include <inttypes.h>

rivr_metrics_t g_rivr_metrics = {0};

void rivr_metrics_print(void)
{
    if (g_rivr_log_mode == RIVR_LOG_SILENT) {
        return;
    }

    printf("@MET {"
           "\"rx_fail\":%" PRIu32 ","
           "\"rx_dup\":%" PRIu32 ","
           "\"rx_ttl\":%" PRIu32 ","
           "\"tx_full\":%" PRIu32 ","
           "\"dc_blk\":%" PRIu32 ","
           "\"fab_drop\":%" PRIu32 ","
           "\"fab_delay\":%" PRIu32 ","
           "\"rad_stall\":%" PRIu32 ","
           "\"rad_txfail\":%" PRIu32 ","
           "\"rad_rst\":%" PRIu32 ","
           "\"rad_crc\":%" PRIu32 ","
           "\"pq_drop\":%" PRIu32 ","
           "\"pq_exp\":%" PRIu32 ","
           "\"pq_peak\":%" PRIu32 ","
           "\"rc_evict\":%" PRIu32 ","
           "\"jitter_ms\":%" PRIu32 ","
           "\"rx_tout\":%" PRIu32 ","
           "\"rst_bkof\":%" PRIu32 ","
           "\"no_route\":%" PRIu32 ","
           "\"rate_lim\":%" PRIu32 ","
           "\"ttl_rel\":%" PRIu32 ","
           "\"txq_peak\":%" PRIu32
           "}\n",
        g_rivr_metrics.rx_decode_fail,
        g_rivr_metrics.rx_dedupe_drop,
        g_rivr_metrics.rx_ttl_drop,
        g_rivr_metrics.tx_queue_full,
        g_rivr_metrics.duty_blocked,
        g_rivr_metrics.fabric_drop,
        g_rivr_metrics.fabric_delay,
        g_rivr_metrics.radio_busy_stall,
        g_rivr_metrics.radio_tx_fail,
        g_rivr_metrics.radio_hard_reset,
        g_rivr_metrics.radio_rx_crc_fail,
        g_rivr_metrics.pq_dropped,
        g_rivr_metrics.pq_expired,
        g_rivr_metrics.pq_peak,
        g_rivr_metrics.rcache_evict,
        g_rivr_metrics.loop_jitter_ms,
        g_rivr_metrics.radio_rx_timeout,
        g_rivr_metrics.radio_reset_backoff,
        g_rivr_metrics.drop_no_route,
        g_rivr_metrics.drop_rate_limited,
        g_rivr_metrics.drop_ttl_relay,
        g_rivr_metrics.tx_queue_peak);
}
