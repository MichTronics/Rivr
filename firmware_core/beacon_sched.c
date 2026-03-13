/**
 * @file  beacon_sched.c
 * @brief Beacon scheduling state machine — see beacon_sched.h for design.
 */

#include "beacon_sched.h"
#include <stddef.h>   /* NULL */

/* ── API ─────────────────────────────────────────────────────────────────── */

void beacon_sched_set_initial_tx(beacon_sched_t *sched, uint32_t now_ms)
{
    if (!sched) return;
    sched->last_tx_ms    = now_ms;
    /* next_min_tx_ms is intentionally left zero here; it will be computed
     * on the first actual timer-fire TX (startup or scheduled). */
}

beacon_sched_result_t beacon_sched_tick(beacon_sched_t *sched,
                                        uint32_t        now_ms,
                                        uint32_t        interval_ms,
                                        uint32_t        jitter_max_ms,
                                        uint8_t         live_neighbor_count,
                                        uint32_t        entropy)
{
    if (!sched) return BEACON_SUPPRESS_INTERVAL;

    /* ── Step 1: boot-window timeout ───────────────────────────────────── *
     * Force startup_done once BEACON_STARTUP_WINDOW_MS has elapsed since   *
     * boot so a delayed or paused timer cannot leave the node stuck in      *
     * startup mode.  Startup count already saturating also triggers this.  */
    if (!sched->startup_done &&
        (now_ms >= BEACON_STARTUP_WINDOW_MS ||
         sched->startup_count >= BEACON_STARTUP_COUNT)) {
        sched->startup_done = true;
    }

    /* ── Step 2: startup burst ─────────────────────────────────────────── */
    if (!sched->startup_done) {
        sched->startup_count++;
        if (sched->startup_count >= BEACON_STARTUP_COUNT) {
            sched->startup_done = true;
        }
        /* Compute jitter for the next (post-startup) interval. */
        uint32_t jitter = (jitter_max_ms > 0u) ? (entropy % jitter_max_ms) : 0u;
        sched->last_tx_ms    = now_ms;
        sched->next_min_tx_ms = interval_ms + jitter;
        return BEACON_TX_STARTUP;
    }

    /* ── Step 3: adaptive suppression ─────────────────────────────────── *
     * Skip the beacon when the neighborhood is already established so that  *
     * dense networks are not flooded with unnecessary announcements.        */
    if (live_neighbor_count >= BEACON_SUPPRESS_MIN_NEIGHBORS) {
        return BEACON_SUPPRESS_NEIGHBORS;
    }

    /* ── Step 4: interval + jitter gate ───────────────────────────────── *
     * After the startup phase, enforce a minimum gap of                     *
     * (interval_ms + jitter_ms) between consecutive beacons to stay well   *
     * within EU868 duty-cycle limits and prevent burst storms.             *
     *                                                                       *
     * last_tx_ms == 0 means no beacon has been sent since the module was   *
     * zero-initialised (rare: only if rivr_sinks_init() was never called). *
     * Allow TX in that case.                                                */
    if (sched->last_tx_ms != 0u) {
        uint32_t elapsed = now_ms - sched->last_tx_ms;
        if (elapsed < sched->next_min_tx_ms) {
            return BEACON_SUPPRESS_INTERVAL;
        }
    }

    /* ── Step 5: permit TX, update state ───────────────────────────────── */
    uint32_t jitter = (jitter_max_ms > 0u) ? (entropy % jitter_max_ms) : 0u;
    sched->last_tx_ms    = now_ms;
    sched->next_min_tx_ms = interval_ms + jitter;
    return BEACON_TX_SCHEDULED;
}
