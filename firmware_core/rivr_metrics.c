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
           "\"fab_delay\":%" PRIu32
           "}\n",
        g_rivr_metrics.rx_decode_fail,
        g_rivr_metrics.rx_dedupe_drop,
        g_rivr_metrics.rx_ttl_drop,
        g_rivr_metrics.tx_queue_full,
        g_rivr_metrics.duty_blocked,
        g_rivr_metrics.fabric_drop,
        g_rivr_metrics.fabric_delay);
}
