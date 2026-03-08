/**
 * @file  fwd_set.h
 * @brief Phase 5 — Opportunistic forward-candidate set.
 *
 * Pure-header module (all functions static inline) that builds a ranked
 * list of the best relay candidates from the live neighbor table and derives
 * two relay-control decisions:
 *
 *   1. **Score-gate suppression** (`fwdset_suppress_relay()`):
 *      Skip relay entirely when we have at least one viable neighbor
 *      (score ≥ FWDSET_MIN_RELAY_SCORE) but our own best direct link is
 *      weaker than that threshold — implying a better-positioned relay
 *      exists.  Edge nodes with *zero* viable neighbors always relay.
 *
 *   2. **Tiered hold-off** (`fwdset_extra_holdoff_ms()`):
 *      Add extra delay to the existing jitter window, scaled by link quality:
 *        • best_direct_score ≥ 70 → +0 ms   (high tier — relay at normal pace)
 *        • best_direct_score ≥ 40 → +50 ms  (mid  tier)
 *        • best_direct_score ≥ 20 → +120 ms (low  tier — Phase 4 opfwd will
 *                                             cancel if a better relay fires)
 *        • suppressed node       → +0 ms   (never reaches enqueue)
 *
 * Integration:
 *   - Include via rivr_embed.h (which also pulls in neighbor_table.h).
 *   - Gated under #if RIVR_FEATURE_OPPORTUNISTIC_FWD in rivr_sources.c.
 *   - Complements Phase 4 reactive suppression (opfwd_suppress.h):
 *     Phase 5 is *proactive* (link quality), Phase 4 is *reactive*
 *     (overheard relay).  Together they form a two-stage relay filter.
 *
 * Memory:
 *   - fwd_set_t is 20 bytes (stack-allocated, never in BSS).
 *   - No global state, no mutex required.
 */

#pragma once

#include "neighbor_table.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tuning constants ────────────────────────────────────────────────────── */

/** Maximum candidates tracked in the forward set. */
#define FWDSET_MAX              3u

/**
 * Minimum relay score threshold.
 * A node's own best-direct link must meet this score or better to relay.
 * When viable_count > 0 but best_direct_score < this value, relay is
 * suppressed (a better-positioned node exists).
 */
#define FWDSET_MIN_RELAY_SCORE  20u

/** Extra hold-off for mid-quality direct links (score 40–69). */
#define FWDSET_HOLDOFF_MID_MS   50u

/** Extra hold-off for low-quality direct links (score 20–39).
 *  Combined with OPFWD_SUPPRESS_EXPIRY_MS (300 ms), this gives the best
 *  relay time to fire and cancel this node's copy before it transmits. */
#define FWDSET_HOLDOFF_LOW_MS   120u

/* ── Types ───────────────────────────────────────────────────────────────── */

/** A single forward candidate (node + its link score). */
typedef struct {
    uint32_t node_id;
    uint8_t  score;
} fwd_candidate_t;

/**
 * Forward-candidate set.
 *
 * Built from the live neighbor table by fwdset_build().  All fields are
 * read-only after construction; do not modify directly.
 *
 *   candidates[]       Top FWDSET_MAX neighbors by score, descending.
 *   count              Number of valid entries in candidates[].
 *   best_direct_score  Highest score of any NTABLE_FLAG_DIRECT neighbor.
 *   direct_count       Count of active (non-stale) DIRECT neighbors.
 *   viable_count       Count of neighbors with score ≥ FWDSET_MIN_RELAY_SCORE.
 */
typedef struct {
    fwd_candidate_t candidates[FWDSET_MAX];
    uint8_t         count;
    uint8_t         best_direct_score;
    uint8_t         direct_count;
    uint8_t         viable_count;
} fwd_set_t;

/* ── fwdset_build ────────────────────────────────────────────────────────── */

/**
 * Build a forward-candidate set from the live neighbor table.
 *
 * Iterates all NTABLE_SIZE slots; skips empty (neighbor_id == 0) and stale
 * entries (last_seen_ms is more than NTABLE_STALE_MS ago).  Scores each
 * live entry with neighbor_link_score_full(), then insertion-sorts the top
 * FWDSET_MAX into candidates[] (descending by score).
 *
 * @param ntbl    Pointer to the global neighbor table (g_ntable).
 * @param now_ms  Current tb_millis() timestamp.
 * @return        Populated fwd_set_t by value (stack allocation, 20 bytes).
 */
static inline fwd_set_t fwdset_build(const rivr_neighbor_table_t *ntbl,
                                     uint32_t now_ms)
{
    fwd_set_t fs;
    memset(&fs, 0, sizeof(fs));

    for (uint8_t i = 0u; i < NTABLE_SIZE; i++) {
        const rivr_neighbor_t *n = &ntbl->entries[i];

        /* Skip empty slots */
        if (n->neighbor_id == 0u) {
            continue;
        }

        /* Skip stale entries — they are not reliable relay candidates */
        if ((now_ms - n->last_seen_ms) > NTABLE_STALE_MS) {
            continue;
        }

        uint8_t sc = neighbor_link_score_full(n, now_ms);

        /* Track direct-link stats */
        if (n->flags & NTABLE_FLAG_DIRECT) {
            fs.direct_count++;
            if (sc > fs.best_direct_score) {
                fs.best_direct_score = sc;
            }
        }

        /* Track viable count (any neighbor above threshold) */
        if (sc >= FWDSET_MIN_RELAY_SCORE) {
            fs.viable_count++;
        }

        /* Insertion-sort into candidates[FWDSET_MAX] descending */
        if (fs.count < FWDSET_MAX) {
            /* Append then bubble up */
            uint8_t pos = fs.count;
            fs.candidates[pos].node_id = n->neighbor_id;
            fs.candidates[pos].score   = sc;
            fs.count++;
            /* Bubble up */
            while (pos > 0u && fs.candidates[pos].score >
                               fs.candidates[pos - 1u].score) {
                fwd_candidate_t tmp = fs.candidates[pos];
                fs.candidates[pos]  = fs.candidates[pos - 1u];
                fs.candidates[pos - 1u] = tmp;
                pos--;
            }
        } else if (sc > fs.candidates[FWDSET_MAX - 1u].score) {
            /* Replace weakest slot and re-sort last position upward */
            uint8_t pos = FWDSET_MAX - 1u;
            fs.candidates[pos].node_id = n->neighbor_id;
            fs.candidates[pos].score   = sc;
            while (pos > 0u && fs.candidates[pos].score >
                               fs.candidates[pos - 1u].score) {
                fwd_candidate_t tmp = fs.candidates[pos];
                fs.candidates[pos]  = fs.candidates[pos - 1u];
                fs.candidates[pos - 1u] = tmp;
                pos--;
            }
        }
    }

    return fs;
}

/* ── fwdset_suppress_relay ───────────────────────────────────────────────── */

/**
 * Decide whether this node should skip relay based on link quality.
 *
 * Returns true (suppress) only when BOTH conditions hold:
 *   • viable_count > 0  — at least one neighbor is above threshold; and
 *   • best_direct_score < FWDSET_MIN_RELAY_SCORE — this node's own link
 *     is weaker than the viability threshold.
 *
 * Returning false unconditionally when viable_count == 0 ensures that
 * edge nodes (no good neighbours) always relay — preventing partitions.
 *
 * @param fs  Built fwd_set_t (from fwdset_build()).
 * @return    true if relay should be suppressed; false to allow relay.
 */
static inline bool fwdset_suppress_relay(const fwd_set_t *fs)
{
    if (fs->viable_count == 0u) {
        /* No viable neighbours — this node must relay (edge case safety). */
        return false;
    }
    return (fs->best_direct_score < FWDSET_MIN_RELAY_SCORE);
}

/* ── fwdset_extra_holdoff_ms ─────────────────────────────────────────────── */

/**
 * Additional hold-off to add on top of the normal jitter window.
 *
 * Three tiers based on best_direct_score:
 *   ≥ 70  →   0 ms  (high quality — relay at normal jitter pace)
 *   ≥ 40  →  50 ms  (medium quality — slight delay)
 *   ≥ 20  → 120 ms  (low quality — Phase 4 opfwd suppression can cancel)
 *   <  20 →   0 ms  (node is suppressed — extra holdoff irrelevant)
 *
 * Call fwdset_suppress_relay() before this; if suppress returns true, the
 * relay is skipped entirely and this function's return value is unused.
 *
 * @param fs  Built fwd_set_t.
 * @return    Extra milliseconds to add to delay_ms.
 */
static inline uint32_t fwdset_extra_holdoff_ms(const fwd_set_t *fs)
{
    if (fs->best_direct_score >= 70u) {
        return 0u;
    }
    if (fs->best_direct_score >= 40u) {
        return FWDSET_HOLDOFF_MID_MS;
    }
    if (fs->best_direct_score >= FWDSET_MIN_RELAY_SCORE) {
        return FWDSET_HOLDOFF_LOW_MS;
    }
    /* Below threshold — either suppressed or no direct neighbours */
    return 0u;
}

/* ── fwdset_sprint ───────────────────────────────────────────────────────── */

/**
 * Format a human-readable summary of the forward set into buf.
 *
 * Output format:
 *   "viable=N best_direct=S holdoff=Hms cands=[0xID:S 0xID:S ...]"
 *
 * @param fs   Built fwd_set_t.
 * @param buf  Output buffer.
 * @param cap  Buffer capacity in bytes.
 */
static inline void fwdset_sprint(const fwd_set_t *fs, char *buf, uint8_t cap)
{
    int off = snprintf(buf, cap,
                       "viable=%u best_direct=%u holdoff=%lums cands=[",
                       (unsigned)fs->viable_count,
                       (unsigned)fs->best_direct_score,
                       (unsigned long)fwdset_extra_holdoff_ms(fs));

    for (uint8_t i = 0u; i < fs->count && off < (int)cap - 16; i++) {
        off += snprintf(buf + off, (size_t)((int)cap - off),
                        "0x%08lx:%u%s",
                        (unsigned long)fs->candidates[i].node_id,
                        (unsigned)fs->candidates[i].score,
                        (i + 1u < fs->count) ? " " : "");
    }

    if (off < (int)cap - 2) {
        buf[off++] = ']';
        buf[off]   = '\0';
    } else if (cap > 0u) {
        buf[cap - 1u] = '\0';
    }
}

#ifdef __cplusplus
}
#endif
