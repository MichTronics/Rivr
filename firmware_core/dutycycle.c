/**
 * @file  dutycycle.c
 * @brief Sliding-window duty-cycle limiter implementation.
 *
 * ALGORITHM
 * ─────────
 * Maintained: a circular array of (tx_end_ms, toa_us) records for recent TXs.
 * On each check:
 *   1. Scan the history and subtract entries older than DC_WINDOW_MS from used_us.
 *      Mark stale slots by setting tx_end_ms = 0 (sentinel).
 *   2. Check if used_us + toa_us <= DC_BUDGET_US.
 *
 * Complexity: O(DC_HISTORY_CAP) per check – bounded and fast for CAP=64.
 */

#include "dutycycle.h"
#include "timebase.h"
#include <string.h>
#include "esp_log.h"
#include "rivr_log.h"
#include "rivr_metrics.h"

#define TAG "DUTYCYCLE"

/* ── Global context ─────────────────────────────────────────────────────── */
dc_ctx_t g_dc;

/* ── Init ────────────────────────────────────────────────────────────────── */
void dutycycle_init(dc_ctx_t *dc)
{
    memset(dc, 0, sizeof(*dc));
}

/* ── Internal: expire old records ────────────────────────────────────────── */
static void expire_old(dc_ctx_t *dc, uint32_t now_ms)
{
    for (uint32_t i = 0; i < DC_HISTORY_CAP; i++) {
        dc_tx_record_t *r = &dc->history[i];
        if (r->tx_end_ms == 0) continue;  /* already expired / unused */

        /* Unsigned 32-bit subtraction handles 32-bit millis wrap correctly
           as long as no entry is older than 2^31 ms (~24 days). */
        uint32_t age_ms = now_ms - r->tx_end_ms;
        if (age_ms > DC_WINDOW_MS) {
            dc->used_us   -= r->toa_us;
            r->tx_end_ms   = 0;
            r->toa_us      = 0;
        }
    }
}

/* ── Check ───────────────────────────────────────────────────────────────── */
bool dutycycle_check(dc_ctx_t *dc, uint32_t now_ms, uint32_t toa_us)
{
    expire_old(dc, now_ms);

    if (dc->used_us + toa_us <= DC_BUDGET_US) {
        return true;
    }

    /* TX blocked */
    dc->blocked_count++;
    g_rivr_metrics.duty_blocked++;
    dc->total_blocked_us += toa_us;
    ESP_LOGW(TAG, "duty-cycle blocked: used=%lluus budget=%lluus toa=%uus",
             (unsigned long long)dc->used_us,
             (unsigned long long)DC_BUDGET_US,
             toa_us);
    return false;
}

/* ── Record ──────────────────────────────────────────────────────────────── */
void dutycycle_record(dc_ctx_t *dc, uint32_t now_ms, uint32_t toa_us)
{
    /* Find a free slot (tx_end_ms == 0) */
    uint32_t slot = dc->head;
    for (uint32_t i = 0; i < DC_HISTORY_CAP; i++) {
        uint32_t idx = (slot + i) & (DC_HISTORY_CAP - 1u);
        if (dc->history[idx].tx_end_ms == 0) {
            dc->history[idx].tx_end_ms = now_ms ? now_ms : 1u; /* avoid 0 sentinel */
            dc->history[idx].toa_us    = toa_us;
            dc->head = (idx + 1u) & (DC_HISTORY_CAP - 1u);
            break;
        }
    }
    dc->used_us += toa_us;
}

/* ── Remaining budget ────────────────────────────────────────────────────── */
uint64_t dutycycle_remaining_us(const dc_ctx_t *dc)
{
    if (dc->used_us >= DC_BUDGET_US) return 0;
    return DC_BUDGET_US - dc->used_us;
}

/* ── Stats ───────────────────────────────────────────────────────────────── */
void dutycycle_print_stats(const dc_ctx_t *dc)
{
    RIVR_LOGI(TAG,
             "used=%llu/%llu us | blocked=%u (%llu us total)",
             (unsigned long long)dc->used_us,
             (unsigned long long)DC_BUDGET_US,
             dc->blocked_count,
             (unsigned long long)dc->total_blocked_us);
}
