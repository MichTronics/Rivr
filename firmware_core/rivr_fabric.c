/**
 * @file  rivr_fabric.c
 * @brief RIVR Fabric — congestion-aware relay suppression (repeater path).
 *
 * See rivr_fabric.h for the full design description.
 *
 * FUTURE:
 * - Optionally embed fabric score into BEACON payload for neighbour-aware
 *   load sharing (do not implement until routing is stable).
 * - Allow neighbour score comparison to prefer less-congested relay paths.
 * - Add CAD busy-channel ratio as an additional congestion input for more
 *   accurate detection on very dense networks.
 *
 * ── IMPLEMENTATION NOTES ────────────────────────────────────────────────────
 *
 * Sliding window
 * ──────────────
 * FABRIC_BUCKETS (60) × 1-second buckets form a ring.
 * bucket_index(now_ms) = (now_ms / 1000) % FABRIC_BUCKETS
 *
 * When the current second changes, every bucket between the old index and the
 * new index (exclusive) is zeroed.  This naturally expires counts that are
 * more than 60 seconds old without requiring a background timer.
 *
 * Summary counts (rx_total, tx_fail_total, etc.) are recomputed by iterating
 * all 60 buckets on each decide() call — 60 integers, cheap on Xtensa LX6.
 *
 * Rates (per-second) are derived as sum / FABRIC_BUCKETS for smoothing:
 *   rx_per_s ≈ rx_total_last_60s / 60
 *   That gives fractional behaviour without floating point.
 * Integer scale: multiply numerator by 100 before dividing to get 100×rate.
 *
 * Score formula (integer-only, values in units of 100× for precision):
 *   score100 = clamp(
 *       rx_per_s_x100 * 2   (i.e. rx_total * 200 / 60)
 *     + blocked_per_s_x100 * 25
 *     + fail_per_s_x100    * 10,
 *     0, 10000)
 *   score = score100 / 100
 *
 * ── TEST PLAN ───────────────────────────────────────────────────────────────
 *
 * Scenario A — baseline (score < 20):
 *   Normal traffic: a few frames per minute, no DC blocks.
 *   Expected: all relay CHAT/DATA get FABRIC_SEND_NOW.
 *   Log check: no "FABRIC: delay/drop" lines.
 *
 * Scenario B — moderate load (score 20..49):
 *   ~8 rx frames/s sustained or intermittent DC blocks.
 *   Expected: relay CHAT/DATA get FABRIC_DELAY with extra ≤ 300 ms.
 *   Log check: "FABRIC: delay relay pkt_type=1 score=NN extra=NNN ms"
 *   Ensure BEACON/ROUTE_REQ/ACK still get SEND_NOW (check logs show no delay/drop for those).
 *
 * Scenario C — high load (score 50..79):
 *   Repeated DC blocks (blocked_dc_per_s ≥ 2).
 *   Expected: relay CHAT/DATA delayed up to 1000 ms extra.
 *   Log check: extra_delay close to 1000 in log output.
 *
 * Scenario D — saturation (score >= 80):
 *   ≥ 4 DC blocks/s or combined high rx + fails.
 *   Expected: relay CHAT/DATA are dropped.
 *   Log check: "FABRIC: drop relay pkt_type=1 score=NN"
 *   Must NOT see BEACON or ROUTE_* in drop logs.
 *
 * Scenario E — score falls after congestion clears:
 *   After DC blocks stop, wait 60 seconds.
 *   Expected: score returns to < 20, relay resumes without delay.
 */

#include "rivr_fabric.h"
#include "esp_log.h"
#include <string.h>
#include "rivr_metrics.h"
#include "rivr_log.h"

#define TAG_FABRIC "FABRIC"

/* ── Sliding window parameters ─────────────────────────────────────────── */

#define FABRIC_BUCKETS    60u    /* one bucket per second, 60-second window  */

/* ── Per-second bucket ──────────────────────────────────────────────────── */

typedef struct {
    uint16_t rx_count;          /* valid decoded frames received             */
    uint16_t tx_ok_count;
    uint16_t tx_fail_count;
    uint16_t tx_blocked_dc;     /* times dutycycle_check returned false      */
    uint16_t tx_enqueued;
} fabric_bucket_t;

/* ── Module state ───────────────────────────────────────────────────────── */

typedef struct {
    fabric_bucket_t buckets[FABRIC_BUCKETS];
    uint32_t        last_bucket_sec;    /* second index of last update       */
} fabric_state_t;

#if RIVR_FABRIC_REPEATER
static fabric_state_t s_fab;
static uint32_t s_relay_drop_total  = 0u;  /**< lifetime relay DROP  counter */
static uint32_t s_relay_delay_total = 0u;  /**< lifetime relay DELAY counter */
static uint32_t s_fabric_relay_total   = 0u;  /**< all CHAT/DATA relay decisions */
static uint32_t s_fabric_tx_blocked_dc = 0u;  /**< dutycycle blocks (lifetime)  */
static uint32_t s_fabric_tx_fail       = 0u;  /**< TX failures (lifetime)        */
static uint32_t s_fabric_tx_ok         = 0u;  /**< TX successes (lifetime)       */
static uint32_t s_fabric_rx_total      = 0u;  /**< decoded RX frames (lifetime)  */
static uint8_t  s_fabric_last_score    = 0u;  /**< score from last decide call   */
#endif

/* ── Helpers (only emitted in repeater builds) ──────────────────────────── */
#if RIVR_FABRIC_REPEATER

/** Advance the ring: zero any buckets that have aged out since last call. */
static void fabric_advance(uint32_t now_ms)
{
    uint32_t now_sec = now_ms / 1000u;
    if (now_sec == s_fab.last_bucket_sec) return;

    /* Zero all buckets that elapsed since last call (cap at full rotation). */
    uint32_t delta = now_sec - s_fab.last_bucket_sec;
    if (delta > FABRIC_BUCKETS) delta = FABRIC_BUCKETS;

    for (uint32_t d = 1u; d <= delta; d++) {
        uint32_t idx = (s_fab.last_bucket_sec + d) % FABRIC_BUCKETS;
        memset(&s_fab.buckets[idx], 0, sizeof(fabric_bucket_t));
    }
    s_fab.last_bucket_sec = now_sec;
}

/** Return pointer to the current second's bucket (advances ring first). */
static fabric_bucket_t *fabric_current(uint32_t now_ms)
{
    fabric_advance(now_ms);
    return &s_fab.buckets[s_fab.last_bucket_sec % FABRIC_BUCKETS];
}

/**
 * Compute congestion score 0..100 from window totals.
 * Uses integer arithmetic only; 100× internal scaling for sub-1 precision.
 */
static uint8_t fabric_score(uint32_t now_ms)
{
    fabric_advance(now_ms);

    uint32_t rx_total       = 0u;
    uint32_t fail_total     = 0u;
    uint32_t blocked_total  = 0u;

    for (uint8_t i = 0u; i < FABRIC_BUCKETS; i++) {
        rx_total      += s_fab.buckets[i].rx_count;
        fail_total    += s_fab.buckets[i].tx_fail_count;
        blocked_total += s_fab.buckets[i].tx_blocked_dc;
    }

    /* Rates × 100 over the 60-second window (= sum × 100 / 60). */
    uint32_t rx_per_s_x100      = rx_total      * 100u / FABRIC_BUCKETS;
    uint32_t fail_per_s_x100    = fail_total     * 100u / FABRIC_BUCKETS;
    uint32_t blocked_per_s_x100 = blocked_total  * 100u / FABRIC_BUCKETS;

    /* score100 = (rx_per_s * 2 + blocked_per_s * 25 + fail_per_s * 10) * 100 */
    uint32_t score100 =
          rx_per_s_x100      *  2u
        + blocked_per_s_x100 * 25u
        + fail_per_s_x100    * 10u;

    /* score100 is now (score * 100); clamp to [0, 10000] then divide. */
    if (score100 > 10000u) score100 = 10000u;
    return (uint8_t)(score100 / 100u);
}

#endif /* RIVR_FABRIC_REPEATER — static helpers */

/* ── Public API ─────────────────────────────────────────────────────────── */

void rivr_fabric_init(void)
{
#if RIVR_FABRIC_REPEATER
    memset(&s_fab, 0, sizeof(s_fab));
    RIVR_LOGI(TAG_FABRIC, "rivr_fabric_init: enabled (RIVR_FABRIC_REPEATER=1)");
#endif
}

void rivr_fabric_tick(uint32_t now_ms)
{
#if RIVR_FABRIC_REPEATER
    fabric_advance(now_ms);
#else
    (void)now_ms;
#endif
}

void rivr_fabric_on_rx(uint32_t now_ms, int8_t rssi_dbm, uint8_t len)
{
#if RIVR_FABRIC_REPEATER
    (void)rssi_dbm; (void)len;
    fabric_current(now_ms)->rx_count++;
    s_fabric_rx_total++;
#else
    (void)now_ms; (void)rssi_dbm; (void)len;
#endif
}

void rivr_fabric_on_tx_enqueued(uint32_t now_ms, uint32_t toa_us)
{
#if RIVR_FABRIC_REPEATER
    (void)toa_us;
    fabric_current(now_ms)->tx_enqueued++;
#else
    (void)now_ms; (void)toa_us;
#endif
}

void rivr_fabric_on_tx_ok(uint32_t now_ms, uint32_t toa_us)
{
#if RIVR_FABRIC_REPEATER
    (void)toa_us;
    fabric_current(now_ms)->tx_ok_count++;
    s_fabric_tx_ok++;
#else
    (void)now_ms; (void)toa_us;
#endif
}

void rivr_fabric_on_tx_fail(uint32_t now_ms, uint32_t toa_us)
{
#if RIVR_FABRIC_REPEATER
    (void)toa_us;
    fabric_current(now_ms)->tx_fail_count++;
    s_fabric_tx_fail++;
#else
    (void)now_ms; (void)toa_us;
#endif
}

void rivr_fabric_on_tx_blocked_dc(uint32_t now_ms, uint32_t toa_us)
{
#if RIVR_FABRIC_REPEATER
    (void)toa_us;
    fabric_current(now_ms)->tx_blocked_dc++;
    s_fabric_tx_blocked_dc++;
#else
    (void)now_ms; (void)toa_us;
#endif
}

fabric_decision_t rivr_fabric_decide_relay(
    const rivr_pkt_hdr_t *pkt,
    uint32_t              now_ms,
    uint32_t              toa_us,
    uint32_t             *out_extra_delay_ms)
{
    /* Always set output first — avoids any uninitialized read on early return. */
    *out_extra_delay_ms = 0u;

#if !RIVR_FABRIC_REPEATER
    (void)pkt; (void)now_ms; (void)toa_us;
    return FABRIC_SEND_NOW;
#else
    (void)toa_us;

    /* Hard rule: never delay/drop control or infrastructure traffic. */
    if (pkt->pkt_type != PKT_CHAT && pkt->pkt_type != PKT_DATA) {
        return FABRIC_SEND_NOW;
    }

    s_fabric_relay_total++;           /* count every CHAT/DATA relay attempt */
    uint8_t score = fabric_score(now_ms);
    s_fabric_last_score = score;

    /* Stability guard: at extreme saturation (score >= BLACKOUT_GUARD_SCORE)
     * never fully DROP — convert to maximum delay to keep the mesh alive.
     * Without this, a brief storm of DC blocks could silence the repeater
     * entirely for the full 60-second window. */
    if (score >= (uint8_t)RIVR_FABRIC_BLACKOUT_GUARD_SCORE) {
        *out_extra_delay_ms = (uint32_t)RIVR_FABRIC_MAX_EXTRA_DELAY_MS;
        ESP_LOGD(TAG_FABRIC,
                 "guard delay pkt_type=%u src=0x%08lx score=%u (blackout guard)",
                 pkt->pkt_type, (unsigned long)pkt->src_id, score);
        s_relay_delay_total++;
        g_rivr_metrics.fabric_delay++;
        return FABRIC_DELAY;
    }

    if (score >= (uint8_t)RIVR_FABRIC_DROP_THRESHOLD) {
        ESP_LOGD(TAG_FABRIC,
                 "drop relay pkt_type=%u src=0x%08lx score=%u",
                 pkt->pkt_type, (unsigned long)pkt->src_id, score);
        s_relay_drop_total++;
        g_rivr_metrics.fabric_drop++;
        return FABRIC_DROP;
    }

    if (score >= (uint8_t)RIVR_FABRIC_DELAY_THRESHOLD) {
        uint32_t extra = 250u + (uint32_t)(score - (uint8_t)RIVR_FABRIC_DELAY_THRESHOLD) * 10u;
        if (extra > (uint32_t)RIVR_FABRIC_MAX_EXTRA_DELAY_MS)
            extra = (uint32_t)RIVR_FABRIC_MAX_EXTRA_DELAY_MS;
        *out_extra_delay_ms = extra;
        ESP_LOGD(TAG_FABRIC,
                 "delay relay pkt_type=%u src=0x%08lx score=%u extra=%lu ms",
                 pkt->pkt_type, (unsigned long)pkt->src_id, score,
                 (unsigned long)extra);
        s_relay_delay_total++;
        g_rivr_metrics.fabric_delay++;
        return FABRIC_DELAY;
    }

    if (score >= (uint8_t)RIVR_FABRIC_LIGHT_DELAY_THRESHOLD) {
        uint32_t extra = (uint32_t)(score - (uint8_t)RIVR_FABRIC_LIGHT_DELAY_THRESHOLD) * 10u;
        if (extra > 300u) extra = 300u;
        *out_extra_delay_ms = extra;
        ESP_LOGD(TAG_FABRIC,
                 "delay relay pkt_type=%u src=0x%08lx score=%u extra=%lu ms",
                 pkt->pkt_type, (unsigned long)pkt->src_id, score,
                 (unsigned long)extra);
        s_relay_delay_total++;
        g_rivr_metrics.fabric_delay++;
        return FABRIC_DELAY;
    }

    return FABRIC_SEND_NOW;
#endif /* RIVR_FABRIC_REPEATER */
}

void rivr_fabric_get_debug(uint32_t now_ms, fabric_debug_t *out)
{
    if (!out) return;
#if RIVR_FABRIC_REPEATER
    fabric_advance(now_ms);
    uint32_t rx_total      = 0u;
    uint32_t fail_total    = 0u;
    uint32_t blocked_total = 0u;
    for (uint8_t i = 0u; i < FABRIC_BUCKETS; i++) {
        rx_total      += s_fab.buckets[i].rx_count;
        fail_total    += s_fab.buckets[i].tx_fail_count;
        blocked_total += s_fab.buckets[i].tx_blocked_dc;
    }
    out->score              = fabric_score(now_ms);
    out->rx_per_s_x100      = (uint16_t)(rx_total      * 100u / FABRIC_BUCKETS);
    out->blocked_per_s_x100 = (uint16_t)(blocked_total * 100u / FABRIC_BUCKETS);
    out->fail_per_s_x100    = (uint16_t)(fail_total     * 100u / FABRIC_BUCKETS);
    out->relay_drop_total   = s_relay_drop_total;
    out->relay_delay_total  = s_relay_delay_total;
#else
    (void)now_ms;
    memset(out, 0, sizeof(*out));
#endif
}

uint8_t rivr_fabric_get_score(void)
{
#if RIVR_FABRIC_REPEATER
    return s_fabric_last_score;
#else
    return 0u;
#endif
}

uint32_t rivr_fabric_get_relay_delayed(void)
{
#if RIVR_FABRIC_REPEATER
    return s_relay_delay_total;
#else
    return 0u;
#endif
}

uint32_t rivr_fabric_get_relay_dropped(void)
{
#if RIVR_FABRIC_REPEATER
    return s_relay_drop_total;
#else
    return 0u;
#endif
}

uint32_t rivr_fabric_get_relay_total(void)
{
#if RIVR_FABRIC_REPEATER
    return s_fabric_relay_total;
#else
    return 0u;
#endif
}

uint32_t rivr_fabric_get_tx_blocked(void)
{
#if RIVR_FABRIC_REPEATER
    return s_fabric_tx_blocked_dc;
#else
    return 0u;
#endif
}

uint32_t rivr_fabric_get_rx_total(void)
{
#if RIVR_FABRIC_REPEATER
    return s_fabric_rx_total;
#else
    return 0u;
#endif
}

/*
 * ── Test plan (Scenarios A–F, repeater build only) ────────────────────────────
 *
 * 1) Under idle network — verify score remains near 0; [FAB] log shows
 *    relay_total = 0, drop = 0, delay = 0.
 *
 * 2) Under sustained spam traffic (≥ 10 RX frames/s) — verify score rises;
 *    relay CHAT/DATA enter DELAY band before DROP band.
 *
 * 3) When dutycycle blocks occur (≥ 2 blocks/s) — verify score rises sharply
 *    (weight 25× per block vs 2× per RX frame).
 *
 * 4) Verify relay delay increases before drop kicks in:
 *    score 20..49 → DELAY up to 300 ms
 *    score 50..79 → DELAY up to RIVR_FABRIC_MAX_EXTRA_DELAY_MS
 *    score 80..94 → DROP
 *    score ≥ 95   → stability guard: DELAY (max) instead of DROP.
 *
 * 5) Control packets (ACK, BEACON, ROUTE_REQ/RPL, PROG_PUSH) — verify they
 *    never appear in FABRIC delay/drop log lines.
 *
 * 6) Non-repeater build (RIVR_FABRIC_REPEATER=0) — verify all getters return 0
 *    and decide_relay always returns FABRIC_SEND_NOW.
 * ───────────────────────────────────────────────────────────────────────────── */
