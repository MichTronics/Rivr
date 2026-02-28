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
 * On each record:
 *   1. expire_old() is called first to free stale slots.
 *   2. If a free slot is found, write the record.
 *   3. If the buffer is full even after expiry (burst > DC_HISTORY_CAP TX/hour),
 *      the OLDEST active entry is evicted: its airtime is subtracted from
 *      used_us before overwriting.  This keeps used_us consistent instead of
 *      silently accumulating un-expirable airtime (the previous bug).
 *
 * Complexity: O(DC_HISTORY_CAP) per check/record – bounded (CAP=512).
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
    /* Expire stale records first: frees slots so the search below succeeds
     * even when check() was not called immediately before record(). */
    expire_old(dc, now_ms);

    /* Search for a free slot; simultaneously track the oldest active entry
     * so we have an LRU candidate ready if the buffer is still full. */
    uint32_t slot     = dc->head;
    uint32_t free_idx = DC_HISTORY_CAP;  /* DC_HISTORY_CAP = "not found" sentinel */
    uint32_t lru_idx  = 0u;
    uint32_t lru_age  = 0u;

    for (uint32_t i = 0; i < DC_HISTORY_CAP; i++) {
        uint32_t idx = (slot + i) & (DC_HISTORY_CAP - 1u);
        if (dc->history[idx].tx_end_ms == 0u) {
            free_idx = idx;
            break;                           /* fast path: free slot found */
        }
        /* Age = unsigned delta; correct across 32-bit millis wrap. */
        uint32_t age = now_ms - dc->history[idx].tx_end_ms;
        if (age > lru_age) {
            lru_age = age;
            lru_idx = idx;
        }
    }

    if (free_idx == DC_HISTORY_CAP) {
        /* Buffer still full after expiry (burst exceeded DC_HISTORY_CAP TX).
         * Evict the oldest record: subtract its airtime so used_us stays
         * consistent.  Without this the previous bug caused used_us to climb
         * permanently until reboot. */
        dc->used_us -= dc->history[lru_idx].toa_us;
        free_idx     = lru_idx;
    }

    dc->history[free_idx].tx_end_ms = now_ms ? now_ms : 1u; /* avoid 0 sentinel */
    dc->history[free_idx].toa_us    = toa_us;
    dc->head = (free_idx + 1u) & (DC_HISTORY_CAP - 1u);
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
