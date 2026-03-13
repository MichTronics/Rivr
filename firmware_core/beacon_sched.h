/**
 * @file  beacon_sched.h
 * @brief Beacon scheduling state machine for EU868-safe discovery.
 *
 * This module implements the three-phase beacon strategy:
 *
 *   Phase 1 — Startup burst
 *     The first BEACON_STARTUP_COUNT timer fires (within BEACON_STARTUP_WINDOW_MS
 *     of boot) always transmit, regardless of interval or neighbor density.
 *     Combined with an immediate boot-time beacon (sent by rivr_sinks_init()),
 *     this gives a bounded initial discovery window without looping.
 *
 *   Phase 2 — Adaptive suppression
 *     After startup, periodic beacons are suppressed when the node already
 *     has enough live neighbors.  "Live" = seen within NTABLE_STALE_MS (30 s).
 *     If neighbor count >= BEACON_SUPPRESS_MIN_NEIGHBORS, skip the beacon.
 *
 *   Phase 3 — Jittered long-interval keepalive
 *     When not suppressed, beacons are rate-limited to at least
 *     beacon_interval_ms + jitter_ms (jitter derived from per-node entropy
 *     so nodes do not synchronize beacon bursts after a shared power cycle).
 *
 * No heap allocation.  Pure logic — all time inputs are caller-supplied so
 * the module is trivially testable on the host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time constants ─────────────────────────────────────────────── */

/**
 * Timer poll granularity: the RIVR timer source fires every BEACON_POLL_MS ms.
 * This is NOT the on-air interval — actual TX intervals are governed by
 * beacon_sched_tick() using the caller-supplied interval_ms + jitter.
 * Keeping it at 60 s gives 1-minute resolution for adaptive suppression
 * without meaningful overhead on a LoRa node.
 */
#define BEACON_POLL_MS                  60000UL

/**
 * Number of startup-burst timer fires.
 * The first BEACON_STARTUP_COUNT fires after boot always TX (no interval
 * or suppression check).  An additional immediate beacon is sent at init
 * time by rivr_sinks_init(), giving BEACON_STARTUP_COUNT + 1 early beacons.
 * After these fires complete (or BEACON_STARTUP_WINDOW_MS elapses), the
 * scheduler transitions to the long-interval keepalive mode.
 */
#define BEACON_STARTUP_COUNT            2u

/**
 * Boot window within which timer fires are treated as startup.
 * If this deadline passes before BEACON_STARTUP_COUNT fires complete
 * (e.g. timer was paused or delayed), startup_done is forced true
 * so the node cannot be stuck in startup mode indefinitely.
 */
#define BEACON_STARTUP_WINDOW_MS        300000UL   /* 5 minutes */

/**
 * Suppress a periodic beacon when this many live (non-stale) neighbors
 * are already known.  A live neighbor = last seen < NTABLE_STALE_MS (30 s).
 * Set to 2 so a fully isolated node still beacons; suppress only when
 * the neighborhood is clearly established.
 */
#define BEACON_SUPPRESS_MIN_NEIGHBORS   2u

/* ── Decision result ─────────────────────────────────────────────────────── */

/**
 * Result returned by beacon_sched_tick().
 * TX results mean the caller should transmit a beacon.
 * SUPPRESS results mean the caller should increment beacon_suppressed_total.
 */
typedef enum {
    BEACON_TX_STARTUP       = 0,  /**< TX — startup-burst phase                  */
    BEACON_TX_SCHEDULED     = 1,  /**< TX — normal jittered keepalive            */
    BEACON_SUPPRESS_NEIGHBORS = 2,/**< Suppressed: enough live neighbors present */
    BEACON_SUPPRESS_INTERVAL  = 3,/**< Suppressed: interval + jitter not elapsed */
} beacon_sched_result_t;

/* ── Scheduler state ─────────────────────────────────────────────────────── */

/**
 * Beacon scheduling state.
 * One instance per node, stored in BSS (zero-initialised at boot).
 * Do not access fields directly; use beacon_sched_tick() and
 * beacon_sched_set_initial_tx().
 */
typedef struct {
    uint32_t last_tx_ms;       /**< Monotonic ms of last beacon TX; 0 = never    */
    uint32_t next_min_tx_ms;   /**< Minimum elapsed ms required before next TX   */
    uint8_t  startup_count;    /**< Timer-fire startup-burst fires consumed       */
    bool     startup_done;     /**< True once startup phase is complete           */
} beacon_sched_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Record that a boot-time beacon was sent (called from rivr_sinks_init()).
 *
 * This anchors last_tx_ms to boot time so the first timer-fire interval
 * check has a valid reference point.  Does NOT advance startup_count
 * (timer-fire-based startup fires are tracked separately).
 *
 * @param sched   Scheduler state.
 * @param now_ms  Current uptime in milliseconds.
 */
void beacon_sched_set_initial_tx(beacon_sched_t *sched, uint32_t now_ms);

/**
 * Evaluate one timer-fire tick and decide whether to transmit.
 *
 * This is the core of the scheduler.  Call from beacon_sink_cb() on each
 * timer event.  The function atomically updates sched state on TX decisions.
 *
 * Logic (in order):
 *   1. Force startup_done if BEACON_STARTUP_WINDOW_MS has elapsed (boot guard).
 *   2. If not startup_done and startup_count < BEACON_STARTUP_COUNT:
 *        increment startup_count, update last_tx_ms + next_min_tx_ms → STARTUP TX.
 *   3. If live_neighbor_count >= BEACON_SUPPRESS_MIN_NEIGHBORS → SUPPRESS_NEIGHBORS.
 *   4. If elapsed since last TX < next_min_tx_ms → SUPPRESS_INTERVAL.
 *   5. Update last_tx_ms + compute new next_min_tx_ms from interval + jitter → SCHEDULED TX.
 *
 * @param sched               Scheduler state (read/write).
 * @param now_ms              Current uptime in milliseconds.
 * @param interval_ms         Configured minimum beacon interval (policy param).
 * @param jitter_max_ms       Maximum random jitter added to interval (policy param).
 * @param live_neighbor_count Count of live (non-stale) neighbors from
 *                            neighbor_table_link_summary().count.
 * @param entropy             Pseudo-random value (e.g. node_id ^ seq ^ now_ms)
 *                            used to compute per-TX jitter.  The same entropy
 *                            source produces different jitter on each call
 *                            because seq and now_ms vary.
 * @return beacon_sched_result_t decision.
 */
beacon_sched_result_t beacon_sched_tick(beacon_sched_t *sched,
                                        uint32_t        now_ms,
                                        uint32_t        interval_ms,
                                        uint32_t        jitter_max_ms,
                                        uint8_t         live_neighbor_count,
                                        uint32_t        entropy);

#ifdef __cplusplus
}
#endif
