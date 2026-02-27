#include "rivr_metrics.h"
#include "rivr_log.h"
#include <stdio.h>

rivr_metrics_t g_rivr_metrics = {0};

void rivr_metrics_print(void)
{
    if (g_rivr_log_mode == RIVR_LOG_SILENT) {
        return;
    }

    printf("@MET {"
           "\"rx_fail\":%u,"
           "\"rx_dup\":%u,"
           "\"rx_ttl\":%u,"
           "\"tx_full\":%u,"
           "\"dc_blk\":%u,"
           "\"fab_drop\":%u,"
           "\"fab_delay\":%u"
           "}\n",
        g_rivr_metrics.rx_decode_fail,
        g_rivr_metrics.rx_dedupe_drop,
        g_rivr_metrics.rx_ttl_drop,
        g_rivr_metrics.tx_queue_full,
        g_rivr_metrics.duty_blocked,
        g_rivr_metrics.fabric_drop,
        g_rivr_metrics.fabric_delay);
}
