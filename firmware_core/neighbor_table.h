/**
 * @file  neighbor_table.h
 * @brief Standalone neighbor-table subsystem for Rivr firmware.
 *
 * Tracks every direct radio peer seen on-air with per-neighbor link-quality
 * metrics (RSSI, SNR, packet-loss rate) and exposes a scored lookup API used
 * by the routing layer when choosing unicast next-hops.
 *
 * Design notes
 * ─────────────
 *  • Fixed-size BSS storage — no heap allocation.
 *  • RSSI and SNR are maintained as EWMA with α = 1/8
 *    (i.e.  new = (old*7 + sample) / 8).
 *  • Loss rate is estimated from sequence-number gaps: each missed slot
 *    drives the EWMA toward 100 and each received frame drives it toward 0.
 *  • Flags are refreshed lazily at every neighbor_find() / neighbor_best()
 *    call so the STALE flag stays current without a background timer.
 *  • This module complements (and is separate from) the routing-embedded
 *    neighbor_entry_t / neighbor_table_t used by the display/CLI layer.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── Size / timing constants ─────────────────────────────────────────────── */

/** Maximum number of entries in the table. */
#define NTABLE_SIZE          16u

/** Entry is considered expired and reusable after this many ms. */
#define NTABLE_EXPIRY_MS     120000u

/** Entry is flagged STALE (but not yet evicted) after this many ms.
 *  Must be > RIVR_BEACON_INTERVAL_MS so a single slightly-late beacon does
 *  not cause a momentary stale gap.  2× the default 30 s beacon period. */
#define NTABLE_STALE_MS      70000u

/** Maximum sequence-number gap counted as "missed" in one update call.
 *  Gaps larger than this are assumed to be a node reboot or seq rollover;
 *  loss-rate EWMA is not driven by more than this many miss-steps at once. */
#define NTABLE_LOSS_MAX_GAP  15u

/** Minimum quality score (0–100) accepted for quality-gated unicast.
 *  Callers may compare routing_next_hop_score() against this threshold. */
#define NTABLE_SCORE_UNICAST_MIN  20u

/* ── Per-neighbor flags ──────────────────────────────────────────────────── */

/** Peer was reached in a single hop (hop_count == 0 from this node). */
#define NTABLE_FLAG_DIRECT   0x01u

/** Peer has not been heard for NTABLE_STALE_MS (but not yet expired). */
#define NTABLE_FLAG_STALE    0x02u

/** At least one PKT_BEACON has been received from this peer. */
#define NTABLE_FLAG_BEACON   0x04u

/* ── Data structures ─────────────────────────────────────────────────────── */

/**
 * Per-neighbor record.
 *
 * The fields listed first match the public contract requested by the caller;
 * the remaining fields are internal bookkeeping (kept in the struct for
 * cache-friendly access and to avoid a parallel shadow array).
 */
typedef struct {
    /* ── Public / readable fields ────────────────────────────────────────── */
    uint32_t neighbor_id;   /**< Node ID; 0 means empty slot.               */
    int16_t  rssi_avg;      /**< EWMA RSSI, dBm (full int16 range; α=1/8).  */
    int8_t   snr_avg;       /**< EWMA SNR, dB (α=1/8).                      */
    uint8_t  loss_rate;     /**< Estimated packet-loss %, 0–100 (α=1/8).    */
    uint32_t last_seen_ms;  /**< Monotonic ms timestamp of last RX frame.   */
    uint8_t  flags;         /**< Bitmask of NTABLE_FLAG_* values.           */

    /* ── Internal bookkeeping ────────────────────────────────────────────── */
    uint8_t  hop_count;     /**< Minimum observed hop-count for this peer.  */
    uint16_t last_seq;      /**< Last wire-protocol seq seen (loss tracking).*/
    uint32_t rx_ok;         /**< Total frames received from this peer.      */

    /* ── Phase 1: airtime metrics foundation ───────────────────────────── *
     * etx_x8: ETX × 8 in fixed point (integer arithmetic; no float).      *
     *   8   = perfect link (delivery ratio = 100%, ETX = 1.0)             *
     *   16  = 50 % delivery (ETX = 2.0, two transmissions expected)       *
     *   255 = link considered unreachable (< 4 % delivery)                *
     * Recomputed from loss_rate on every neighbor_update() call.          *
     * Used by neighbor_link_score_full() when                             *
     *   RIVR_FEATURE_AIRTIME_ROUTING = 1 (Phase 2+).                      *
     *
     * avg_frame_len: EWMA of observed wire frame length in bytes (α=1/8). *
     * Used by Phase 3 adaptive-flood control for ToA estimation.          */
    uint8_t  etx_x8;        /**< ETX×8 fixed-point quality; 8=best, 255=dead. */
    uint8_t  avg_frame_len; /**< EWMA wire frame length (bytes); 0=no data.   */
} rivr_neighbor_t;

/** Fixed-size table of rivr_neighbor_t entries. */
typedef struct {
    rivr_neighbor_t entries[NTABLE_SIZE];
    uint8_t         count;  /**< Number of active (non-empty) entries.      */
} rivr_neighbor_table_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * Initialise (zero-out) the neighbor table.
 *
 * Must be called once before any other function.
 *
 * @param tbl  Pointer to the table to initialise.
 */
void neighbor_table_init(rivr_neighbor_table_t *tbl);

/**
 * Record or refresh a peer observation.
 *
 * Looks up @p node_id; if found, updates EWMA metrics and loss-rate.
 * If not found, allocates a new slot (evicting the oldest entry when the
 * table is full).
 *
 * On the very first observation of a node the EWMA fields are seeded with
 * the raw sample values so the first reading is immediately useful.
 *
 * @param tbl        Table to update.
 * @param node_id    Wire-protocol source node ID of the observed peer.
 * @param rssi_dbm   Received RSSI in dBm (int16 for wide-range radios).
 * @param snr_db     Received SNR in dB.
 * @param hop_count  Hop field from the received packet header.
 * @param seq        Wire-protocol sequence number (for loss estimation).
 * @param frame_len  Wire frame length in bytes (updates avg_frame_len EWMA);
 *                   pass 0 if the length is unknown (no EWMA update).
 * @param now_ms     Current monotonic timestamp in milliseconds.
 * @return Pointer to the updated entry, or NULL if @p node_id is 0.
 */
rivr_neighbor_t *neighbor_update(rivr_neighbor_table_t *tbl,
                                 uint32_t               node_id,
                                 int16_t                rssi_dbm,
                                 int8_t                 snr_db,
                                 uint8_t                hop_count,
                                 uint16_t               seq,
                                 uint8_t                frame_len,
                                 uint32_t               now_ms);

/**
 * Look up a peer by node ID.
 *
 * Updates the NTABLE_FLAG_STALE flag based on @p now_ms.
 *
 * @param tbl      Table to search (const — no write side-effects on metrics).
 * @param node_id  Node ID to find.
 * @param now_ms   Current monotonic timestamp (used for expiry/stale checks).
 * @return Pointer to the entry (may have STALE flag set), or NULL if the
 *         peer is not in the table or its entry has expired.
 *
 * @note The stale-flag write is performed through a const-cast because it is
 *       a pure derived property of existing data and has no observable effect
 *       on the metric values; callers that need read-only access should treat
 *       the returned pointer as logically const.
 */
const rivr_neighbor_t *neighbor_find(const rivr_neighbor_table_t *tbl,
                                     uint32_t                     node_id,
                                     uint32_t                     now_ms);

/**
 * Return the non-expired neighbor with the highest link-quality score.
 *
 * @param tbl     Table to search.
 * @param now_ms  Current monotonic timestamp.
 * @return Pointer to the best-scoring entry, or NULL if the table is empty
 *         or all entries have expired.
 */
const rivr_neighbor_t *neighbor_best(const rivr_neighbor_table_t *tbl,
                                     uint32_t                     now_ms);

/**
 * Compute the composite link-quality score for one neighbor entry.
 *
 * Score derivation (all components 0–100):
 *   rssi_part  = clamp(rssi_avg + 140, 0, 80)
 *   snr_part   = clamp(snr_avg  +  10, 0, 20)
 *   base       = rssi_part + snr_part            — 0..100
 *   loss_pen   = loss_rate                        — 0..100
 *   after_loss = base × (100 − loss_pen) / 100   — 0..100
 *   score      = after_loss × (EXPIRY − age) / EXPIRY  (linear age decay)
 *
 * @param n       Neighbor entry (may be NULL → returns 0).
 * @param now_ms  Current monotonic timestamp.
 * @return Quality score in [0, 100].
 */
uint8_t neighbor_link_score(const rivr_neighbor_t *n, uint32_t now_ms);

/**
 * Compute the ETX-aware composite link-quality score for one neighbor.
 *
 * When RIVR_FEATURE_AIRTIME_ROUTING == 1 (Phase 2+):
 *   Uses ETX × 8 weighting instead of the linear loss-rate penalty:
 *
 *     rssi_part  = clamp(rssi_avg + 140, 0, 80)
 *     snr_part   = clamp(snr_avg  +  10, 0, 20)
 *     base       = rssi_part + snr_part               — 0..100
 *     etx_scaled = base × 8 / etx_x8                 — 0..100
 *                  (etx_x8=8 → ×1.0, etx_x8=16 → ×0.5)
 *     score      = etx_scaled × (EXPIRY − age) / EXPIRY
 *
 * When RIVR_FEATURE_AIRTIME_ROUTING == 0 (default):
 *   Identical to neighbor_link_score() — no behavior change.
 *
 * The routing layer calls this function (rather than neighbor_link_score)
 * so that flipping the feature flag upgrades scoring transparently.
 *
 * @param n       Neighbor entry (may be NULL → returns 0).
 * @param now_ms  Current monotonic timestamp.
 * @return Quality score in [0, 100].
 */
uint8_t neighbor_link_score_full(const rivr_neighbor_t *n, uint32_t now_ms);

/**
 * Set one or more flag bits on an existing neighbor entry.
 *
 * A typical use is setting NTABLE_FLAG_BEACON when a PKT_BEACON is received.
 * No-op if @p node_id is not in the table.
 *
 * @param tbl      Table to update.
 * @param node_id  Target peer.
 * @param flag     One or more NTABLE_FLAG_* bits to OR into the entry.
 */
void neighbor_set_flag(rivr_neighbor_table_t *tbl,
                       uint32_t               node_id,
                       uint8_t                flag);

/**
 * Remove all entries older than NTABLE_EXPIRY_MS.
 *
 * Compacts the table in-place (swaps each expired entry with the last active
 * entry, then decrements count).
 *
 * @param tbl     Table to expire.
 * @param now_ms  Current monotonic timestamp.
 * @return Number of entries removed.
 */
uint8_t neighbor_table_expire(rivr_neighbor_table_t *tbl, uint32_t now_ms);

/**
 * Print the standalone neighbor table as a human-readable table.
 *
 * Column headers:
 *   NodeID  RSSI  SNR  Loss%  ETX×8  AvgLen  ScoreFull  Age(s)  rx_ok  Flags
 *
 * The ETX×8 column shows the Phase 1 quality indicator:
 *   8  = perfect link (100% delivery)
 *   16 = 50% delivery (ETX = 2.0)
 *   255 = effectively dead link
 *
 * Flags shown as compact characters: D=direct S=stale B=beacon
 *
 * @param tbl     Table to print.
 * @param now_ms  Current monotonic timestamp in milliseconds.
 */
void neighbor_table_print(const rivr_neighbor_table_t *tbl, uint32_t now_ms);

/* ── Per-print link quality summary ─────────────────────────────────────── */

/**
 * Aggregate link-quality snapshot across all live (non-stale) neighbors.
 * Computed on demand at metrics-print time; O(NTABLE_SIZE) with no allocation.
 */
typedef struct {
    uint8_t count;       /**< Live neighbor count (age < NTABLE_STALE_MS)    */
    uint8_t best_score;  /**< Highest neighbor_link_score_full (0–100)        */
    int8_t  best_rssi;   /**< EWMA RSSI of the best-scoring neighbor (dBm)   */
    uint8_t avg_loss;    /**< Average loss_rate % across all live neighbors   */
} neighbor_link_summary_t;

/**
 * Compute the link quality summary from a neighbor table in O(NTABLE_SIZE).
 * Safe to call from the main loop every 5 s; all integer math, no alloc.
 *
 * @param tbl     Pointer to the global neighbor table.
 * @param now_ms  Current tb_millis() timestamp.
 * @return        Populated neighbor_link_summary_t by value (4 bytes).
 */
static inline neighbor_link_summary_t
neighbor_table_link_summary(const rivr_neighbor_table_t *tbl, uint32_t now_ms)
{
    neighbor_link_summary_t s;
    s.count      = 0u;
    s.best_score = 0u;
    s.best_rssi  = -120;   /* sentinel: updated on first live neighbor */
    s.avg_loss   = 0u;

    if (!tbl) return s;

    uint32_t loss_sum = 0u;

    for (uint8_t _i = 0u; _i < NTABLE_SIZE; _i++) {
        const rivr_neighbor_t *_n = &tbl->entries[_i];
        if (_n->neighbor_id == 0u) continue;
        if ((now_ms - _n->last_seen_ms) >= NTABLE_STALE_MS) continue;  /* stale */

        uint8_t _sc = neighbor_link_score_full(_n, now_ms);
        s.count++;
        loss_sum += _n->loss_rate;

        if (_sc >= s.best_score) {   /* >= so first entry always wins */
            s.best_score = _sc;
            /* Clamp int16_t rssi_avg to int8_t range (practical LoRa RSSI */
            /* is always in [-120, -20] which fits, but be defensive). */
            int16_t _r = _n->rssi_avg;
            s.best_rssi = (int8_t)(_r < -128 ? -128 : _r > 127 ? 127 : _r);
        }
    }

    if (s.count > 0u) {
        s.avg_loss = (uint8_t)(loss_sum / s.count);
    }
    return s;
}
