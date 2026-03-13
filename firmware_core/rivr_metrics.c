#include "rivr_metrics.h"
#include "rivr_log.h"
#include <stdio.h>
#include <inttypes.h>

rivr_metrics_t g_rivr_metrics = {0};

void rivr_metrics_print(const rivr_live_stats_t *live)
{
    if (g_rivr_log_mode == RIVR_LOG_SILENT) {
        return;
    }

    const rivr_live_stats_t zero = {0};
    if (!live) live = &zero;

    printf("@MET {"
           "\"node_id\":%" PRIu32 ","
           "\"dc_pct\":%" PRIu8 ","
           "\"q_depth\":%" PRIu8 ","
           "\"tx_total\":%" PRIu32 ","
           "\"rx_total\":%" PRIu32 ","
           "\"route_cache\":%" PRIu8 ","
           "\"rx_fail\":%" PRIu32 ","
           "\"rx_dup\":%" PRIu32 ","
           "\"rx_ttl\":%" PRIu32 ","
           "\"rx_bad_type\":%" PRIu32 ","
           "\"rx_bad_hop\":%" PRIu32 ","
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
           "\"txq_peak\":%" PRIu32 ","
           "\"at_low\":%" PRIu32 ","
           "\"cls_ctrl\":%" PRIu32 ","
           "\"cls_chat\":%" PRIu32 ","
           "\"cls_met\":%" PRIu32 ","
           "\"cls_bulk\":%" PRIu32 ","
           "\"rad_busy_tmo\":%" PRIu32 ","
           "\"tx_tmo\":%" PRIu32 ","
           "\"tx_ddl\":%" PRIu32 ","
           "\"rst_busy\":%" PRIu32 ","
           "\"rst_txtmo\":%" PRIu32 ","
           "\"rst_spurious\":%" PRIu32 ","
           "\"rst_rxtmo\":%" PRIu32 ","
           "\"loop_drop\":%" PRIu32 ","
           "\"rreq_rx\":%" PRIu32 ","
           "\"rreq_sent\":%" PRIu32 ","
           "\"rreq_cache\":%" PRIu32 ","
           "\"rreq_target\":%" PRIu32 ","
           "\"rreq_supp\":%" PRIu32 ","
           "\"rrpl_rx\":%" PRIu32 ","
           "\"rrpl_learn\":%" PRIu32 ","
           "\"pq_drain\":%" PRIu32 ","
           "\"pq_exp_total\":%" PRIu32 ","
           "\"loop_drop_total\":%" PRIu32 ","
           "\"rc_miss\":%" PRIu32 ","
           "\"rc_hit\":%" PRIu32 ","
           "\"fwd_ttl_drop\":%" PRIu32 ","
           "\"ack_tx\":%" PRIu32 ","
           "\"ack_rx\":%" PRIu32 ","
           "\"retry_att\":%" PRIu32 ","
           "\"retry_ok\":%" PRIu32 ","
           "\"retry_fail\":%" PRIu32 ","
           "\"retry_fb\":%" PRIu32 ","
           "\"fb_flood\":%" PRIu32 ","
           "\"nb_route_ok\":%" PRIu32 ","
           "\"nb_route_fail\":%" PRIu32 ","
           "\"fwd_att\":%" PRIu32 ","
           "\"fwd_opc\":%" PRIu32 ","
           "\"fwd_scs\":%" PRIu32 ","
           "\"at_sel\":%" PRIu32 ","
           "\"at_fb\":%" PRIu32 ","
           "\"relay_sel\":%" PRIu32 ","
           "\"relay_can\":%" PRIu32 ","
           "\"relay_fwd\":%" PRIu32 ","
           /* real-time link quality snapshot (4 fields from g_ntable) */
           "\"lnk_cnt\":%" PRIu8 ","
           "\"lnk_best\":%" PRIu8 ","
           "\"lnk_rssi\":%" PRId8 ","
           "\"lnk_loss\":%" PRIu8 ","
           /* adaptive relay observability */
           "\"relay_skip\":%" PRIu32 ","
           "\"relay_delay\":%" PRIu32 ","
           "\"relay_density\":%" PRIu8 ","
           /* beacon observability */
           "\"bcn_tx\":%" PRIu32 ","
           "\"bcn_start\":%" PRIu32 ","
           "\"bcn_supp\":%" PRIu32 ","
           "\"bcn_drop\":%" PRIu32
           "}\n",
        live->node_id,
        live->dc_pct,
        live->q_depth,
        live->tx_total,
        live->rx_total,
        live->route_cache,
        g_rivr_metrics.rx_decode_fail,
        g_rivr_metrics.rx_dedupe_drop,
        g_rivr_metrics.rx_ttl_drop,
        g_rivr_metrics.rx_invalid_type,
        g_rivr_metrics.rx_invalid_hop,
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
        g_rivr_metrics.tx_queue_peak,
        g_rivr_metrics.airtime_tokens_low,
        g_rivr_metrics.class_drops_ctrl,
        g_rivr_metrics.class_drops_chat,
        g_rivr_metrics.class_drops_metrics,
        g_rivr_metrics.class_drops_bulk,
        g_rivr_metrics.radio_busy_timeout_total,
        g_rivr_metrics.tx_timeout_total,
        g_rivr_metrics.tx_deadline_total,
        g_rivr_metrics.radio_reset_busy_stuck,
        g_rivr_metrics.radio_reset_tx_timeout,
        g_rivr_metrics.radio_reset_spurious_irq,
        g_rivr_metrics.radio_reset_rx_timeout,
        g_rivr_metrics.loop_detect_drop,
        g_rivr_metrics.route_req_rx_total,
        g_rivr_metrics.route_req_reply_sent_total,
        g_rivr_metrics.route_req_reply_cache_total,
        g_rivr_metrics.route_req_reply_target_total,
        g_rivr_metrics.route_req_reply_suppressed_total,
        g_rivr_metrics.route_rpl_rx_total,
        g_rivr_metrics.route_rpl_learn_total,
        g_rivr_metrics.pending_queue_drained_total,
        g_rivr_metrics.pending_queue_expired_total,
        g_rivr_metrics.loop_detect_drop_total,
        g_rivr_metrics.route_cache_miss_total,
        g_rivr_metrics.route_cache_hit_total,
        g_rivr_metrics.forward_drop_ttl_total,
        g_rivr_metrics.ack_tx_total,
        g_rivr_metrics.ack_rx_total,
        g_rivr_metrics.retry_attempt_total,
        g_rivr_metrics.retry_success_total,
        g_rivr_metrics.retry_fail_total,
        g_rivr_metrics.retry_fallback_total,
        g_rivr_metrics.fallback_flood_total,
        g_rivr_metrics.neighbor_route_used_total,
        g_rivr_metrics.neighbor_route_failed_total,
        g_rivr_metrics.flood_fwd_attempted_total,
        g_rivr_metrics.flood_fwd_cancelled_opport_total,
        g_rivr_metrics.flood_fwd_score_suppressed_total,
        g_rivr_metrics.airtime_route_selected_total,
        g_rivr_metrics.airtime_route_fallback_total,
        /* opportunistic relay: selected / cancelled / forwarded */
        g_rivr_metrics.flood_fwd_attempted_total,
        g_rivr_metrics.flood_fwd_cancelled_opport_total,
        g_rivr_metrics.relay_forwarded_total,
        /* link quality snapshot */
        live->lnk_cnt,
        live->lnk_best,
        live->lnk_best_rssi,
        live->lnk_avg_loss,
        /* adaptive relay: relay_skip = phase-4 + phase-5 suppress totals */
        g_rivr_metrics.flood_fwd_cancelled_opport_total
            + g_rivr_metrics.flood_fwd_score_suppressed_total,
        g_rivr_metrics.relay_delay_ms_total,
        live->relay_density,
        /* beacon observability */
        g_rivr_metrics.beacon_tx_total,
        g_rivr_metrics.beacon_startup_tx_total,
        g_rivr_metrics.beacon_suppressed_total,
        g_rivr_metrics.beacon_class_drop);
}
