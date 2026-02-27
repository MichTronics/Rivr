/**
 * @file  build_info.c
 * @brief Build identity printing — boot banner + JSON export.
 *
 * No heap, no ESP_LOG prefix on the banner line (pure printf so it appears
 * on the serial monitor without color codes or chip-tick timestamps).
 *
 * Deps: stdio.h, inttypes.h, rivr_metrics.h (for supportpack JSON only).
 */

#include "build_info.h"
#include "rivr_metrics.h"
#include "rivr_log.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ── Feature flags string (evaluated once at link time) ─────────────────── */

static const char k_role[]    = _RIVR_ROLE_TAG;
static const char k_radio[]   = _RIVR_RADIO_TAG;
static const char k_fabric[]  = _RIVR_FABRIC_TAG;
static const char k_sim[]     = _RIVR_SIM_TAG;

/* ── Public: boot banner ─────────────────────────────────────────────────── */

void build_info_print_banner(void)
{
    /* Single line, no ESP_LOG prefix, \r\n for serial monitors that need CR. */
    printf("[RIVR] env=%s sha=%s built=%s %s"
           " role=%s radio=%s freq=%" PRIu32 " SF%u BW%ukHz CR4/%u"
           " cc=%s flags=%s%s\r\n",
           RIVR_BUILD_ENV,
           RIVR_GIT_SHA,
           __DATE__, __TIME__,
           k_role,
           k_radio,
           (uint32_t)RIVR_RF_FREQ_HZ,
           (unsigned)RF_SPREADING_FACTOR,
           (unsigned)RF_BANDWIDTH_KHZ,
           (unsigned)RF_CODING_RATE,
           RIVR_COMPILER_VER,
           k_fabric,
           k_sim);
    fflush(stdout);
}

/* ── Public: JSON export ─────────────────────────────────────────────────── */

int build_info_write_json(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0u) return 0;

    int n = snprintf(buf, buf_len,
        "{"
        "\"env\":\"%s\","
        "\"sha\":\"%s\","
        "\"built\":\"%s %s\","
        "\"role\":\"%s\","
        "\"radio\":\"%s\","
        "\"freq\":%" PRIu32 ","
        "\"sf\":%u,"
        "\"bw_khz\":%u,"
        "\"cr\":\"4/%u\","
        "\"fabric\":%u,"
        "\"sim\":%u,"
        "\"cc\":\"%s\","
        "\"met\":{"
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
            "\"rst_rxtmo\":%" PRIu32
        "}"
        "}",
        RIVR_BUILD_ENV,
        RIVR_GIT_SHA,
        __DATE__, __TIME__,
        k_role,
        k_radio,
        (uint32_t)RIVR_RF_FREQ_HZ,
        (unsigned)RF_SPREADING_FACTOR,
        (unsigned)RF_BANDWIDTH_KHZ,
        (unsigned)RF_CODING_RATE,
        (unsigned)RIVR_FABRIC_REPEATER,
        (unsigned)
#ifdef RIVR_SIM_MODE
        1u,
#else
        0u,
#endif
        RIVR_COMPILER_VER,
        /* metrics snapshot */
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
        g_rivr_metrics.tx_queue_peak,
        g_rivr_metrics.airtime_tokens_low,
        g_rivr_metrics.class_drops_ctrl,
        g_rivr_metrics.class_drops_chat,
        g_rivr_metrics.class_drops_metrics,
        g_rivr_metrics.class_drops_bulk,
        /* Step-9 counters */
        g_rivr_metrics.radio_busy_timeout_total,
        g_rivr_metrics.tx_timeout_total,
        g_rivr_metrics.tx_deadline_total,
        g_rivr_metrics.radio_reset_busy_stuck,
        g_rivr_metrics.radio_reset_tx_timeout,
        g_rivr_metrics.radio_reset_spurious_irq,
        g_rivr_metrics.radio_reset_rx_timeout
    );

    /* snprintf returns the number of chars it *would* have written;
     * clamp to the actual bytes in buf (minus NUL). */
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    int written = (n < (int)buf_len) ? n : (int)(buf_len - 1u);
    buf[written] = '\0';
    return written;
}

/* ── Public: supportpack JSON export ────────────────────────────────────────── */

int build_info_write_supportpack(char    *buf,
                                 size_t   buf_len,
                                 uint8_t  neighbor_cnt,
                                 uint8_t  route_cnt,
                                 uint8_t  pending_cnt,
                                 uint64_t dc_remaining_us,
                                 uint64_t dc_used_us,
                                 uint32_t dc_blocked,
                                 uint32_t uptime_ms,
                                 uint32_t rx_frames,
                                 uint32_t tx_frames)
{
    if (!buf || buf_len == 0u) return 0;

    int n = snprintf(buf, buf_len,
        "{"
        "\"env\":\"%s\","
        "\"sha\":\"%s\","
        "\"built\":\"%s %s\","
        "\"role\":\"%s\","
        "\"radio\":\"%s\","
        "\"freq\":%" PRIu32 ","
        "\"sf\":%u,"
        "\"bw_khz\":%u,"
        "\"cr\":\"4/%u\","
        "\"fabric\":%u,"
        "\"sim\":%u,"
        "\"uptime_ms\":%" PRIu32 ","
        "\"rx_frames\":%" PRIu32 ","
        "\"tx_frames\":%" PRIu32 ","
        "\"routing\":{"
            "\"neighbors\":%u,"
            "\"routes\":%u,"
            "\"pending\":%u"
        "},"
        "\"dc\":{"
            "\"remaining_us\":%" PRIu64 ","
            "\"used_us\":%" PRIu64 ","
            "\"blocked\":%" PRIu32
        "},"
        "\"met\":{"
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
            "\"rst_rxtmo\":%" PRIu32
        "}"
        "}",
        /* build identity */
        RIVR_BUILD_ENV, RIVR_GIT_SHA, __DATE__, __TIME__,
        k_role, k_radio,
        (uint32_t)RIVR_RF_FREQ_HZ,
        (unsigned)RF_SPREADING_FACTOR,
        (unsigned)RF_BANDWIDTH_KHZ,
        (unsigned)RF_CODING_RATE,
        (unsigned)
#ifndef RIVR_FABRIC_REPEATER
        0u,
#else
        (unsigned)RIVR_FABRIC_REPEATER,
#endif
        (unsigned)
#ifdef RIVR_SIM_MODE
        1u,
#else
        0u,
#endif
        /* runtime totals */
        uptime_ms, rx_frames, tx_frames,
        /* routing summary */
        (unsigned)neighbor_cnt,
        (unsigned)route_cnt,
        (unsigned)pending_cnt,
        /* duty-cycle snapshot */
        dc_remaining_us, dc_used_us, dc_blocked,
        /* metrics */
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
        g_rivr_metrics.tx_queue_peak,
        g_rivr_metrics.airtime_tokens_low,
        g_rivr_metrics.class_drops_ctrl,
        g_rivr_metrics.class_drops_chat,
        g_rivr_metrics.class_drops_metrics,
        g_rivr_metrics.class_drops_bulk,
        /* Step-9 counters */
        g_rivr_metrics.radio_busy_timeout_total,
        g_rivr_metrics.tx_timeout_total,
        g_rivr_metrics.tx_deadline_total,
        g_rivr_metrics.radio_reset_busy_stuck,
        g_rivr_metrics.radio_reset_tx_timeout,
        g_rivr_metrics.radio_reset_spurious_irq,
        g_rivr_metrics.radio_reset_rx_timeout
    );

    if (n < 0) { buf[0] = '\0'; return 0; }
    int written = (n < (int)buf_len) ? n : (int)(buf_len - 1u);
    buf[written] = '\0';
    return written;
}
