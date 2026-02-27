/**
 * @file  replay.h
 * @brief Deterministic replay / sim harness for RIVR C-layer unit tests.
 *
 * ── Trace format (JSONL) ────────────────────────────────────────────────────
 *
 * Each non-blank, non-comment line in a trace file is a JSON object whose
 * shape is determined by the "ev" key.  Supported event types:
 *
 *  {"ev":"rx_frame","t_ms":N,"src":"0xAAAA0001","dst":"0x00000000",
 *   "seq":N,"type":N,"ttl":7,"hop":0,"rssi":-70,"snr":8,
 *   "payload":"hello","fwd_toa_us":50000}
 *      Simulate receiving a wire-format frame.  The runner encodes the fields
 *      into a real wire frame, optionally corrupts the CRC (if a crc_fail fault
 *      is armed), then runs it through: protocol_decode → routing_flood_forward
 *      → routing_neighbor_update → route_cache_learn_rx →
 *      airtime_sched_check_consume → rb_try_push(rf_tx_queue).
 *      fwd_toa_us is optional (default: REPLAY_DEFAULT_TOA_US).
 *
 *  {"ev":"tick","t_ms":N}
 *      Advance the simulated clock to N ms.  All subsequent routing and
 *      neighbour calls receive N as now_ms.
 *
 *  {"ev":"fault","name":"crc_fail","count":N}
 *      Arm N burst CRC-error injections.  The next N rx_frame events will have
 *      their wire CRC intentionally corrupted before protocol_decode, causing
 *      rx_decode_fail and radio_rx_crc_fail to increment.
 *
 *  {"ev":"fault","name":"busy_stuck","count":N}
 *      Arm N radio-busy-stall injections.  The next N frames that would be
 *      forwarded to the TX queue instead increment radio_busy_stall and are
 *      discarded.
 *
 *  {"ev":"fault","name":"tx_timeout","count":N}
 *      Arm N TX-timeout injections.  Mirrored to radio_tx_fail metric.
 *
 *  {"ev":"fault","name":"queue_fill","count":N}
 *      Push N dummy frames directly into rf_tx_queue.  Used to pre-fill the
 *      queue so subsequent forward attempts observe tx_queue_full.
 *
 *  {"ev":"fault","name":"drain_tokens"}
 *      Set the global airtime token bucket to zero (simulates a burst of prior
 *      transmissions depleting the budget).  next_refill anchored to now_ms.
 *
 *  {"ev":"assert","what":"metric","key":"NAME","op":"OP","val":N}
 *      Assert g_rivr_metrics.NAME <OP> N.
 *      Supported OP: "eq", "ne", "gt", "gte", "lt".
 *
 *  {"ev":"assert","what":"neighbor_count","op":"OP","val":N}
 *      Assert routing_neighbor_count(nb, now_ms) <OP> N.
 *
 *  {"ev":"assert","what":"route","node":"0xXXX","op":"exists"}
 *  {"ev":"assert","what":"route","node":"0xXXX","op":"absent"}
 *      Assert presence / absence of a live route to the given dst_id.
 *
 *  {"ev":"comment","msg":"..."}
 *      Human-readable note; printed to stdout and otherwise ignored.
 *
 * ── Usage ───────────────────────────────────────────────────────────────────
 *
 *  replay_ctx_t ctx;
 *  replay_ctx_init(&ctx);
 *  int failures = replay_run_file(&ctx, "traces/multihop.jsonl");
 *  // failures == 0 → all assertions passed
 */

#pragma once
#include <stdint.h>

#include "routing.h"        /* dedupe_cache_t, neighbor_table_t, forward_budget_t */
#include "route_cache.h"    /* route_cache_t                                       */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tuning ─────────────────────────────────────────────────────────────────── */

/** Default time-on-air when rx_frame event does not specify fwd_toa_us (µs). */
#define REPLAY_DEFAULT_TOA_US   50000u

/** Maximum trace line length handled by the parser. */
#define REPLAY_MAX_LINE         512u

/* ── Fault state ────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t crc_fail_remaining;   /**< Corrupt CRC on next N rx_frame events  */
    uint32_t busy_stuck_remaining; /**< radio_busy_stall on next N TX attempts  */
    uint32_t tx_timeout_remaining; /**< radio_tx_fail on next N TX attempts     */
} replay_fault_t;

/* ── Harness context ────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t         now_ms;   /**< Simulated monotonic clock (ms)             */

    /* Routing layer state */
    dedupe_cache_t   dc;
    neighbor_table_t nb;
    route_cache_t    rc;
    forward_budget_t fb;

    /* Active fault injections */
    replay_fault_t   fault;

    /* Per-file assertion counters (reset by replay_run_file on entry) */
    uint32_t         pass;
    uint32_t         fail;
} replay_ctx_t;

/* ── API ────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise a replay context to a clean state.
 *
 * Zeros all routing caches, resets clock to 0, clears fault state.
 * Reinitialises the global airtime bucket (airtime_sched_init()).
 * Resets g_rivr_metrics to zero.
 *
 * Call once before each independent trace run to avoid state bleeding between
 * scenarios.
 */
void replay_ctx_init(replay_ctx_t *ctx);

/**
 * @brief Run a JSONL trace file through the replay harness.
 *
 * Opens @p path, reads it line by line, dispatches each event type, and
 * evaluates all assert events.  Results are accumulated in ctx->pass /
 * ctx->fail AND printed to stdout in the same "  OK  / FAIL" style as the
 * acceptance suite.
 *
 * @param ctx   Harness context (must be initialised with replay_ctx_init).
 * @param path  Path to a .jsonl trace file.
 * @return Number of failed assertions in this file (0 = all pass).
 */
int replay_run_file(replay_ctx_t *ctx, const char *path);

#ifdef __cplusplus
}
#endif
