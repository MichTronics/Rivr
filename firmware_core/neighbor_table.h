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

/** Entry is flagged STALE (but not yet evicted) after this many ms. */
#define NTABLE_STALE_MS      30000u

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
    uint8_t  _pad[2];       /**< Padding to 24 bytes (cache-line friendly). */
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
 * @param now_ms     Current monotonic timestamp in milliseconds.
 * @return Pointer to the updated entry, or NULL if @p node_id is 0.
 */
rivr_neighbor_t *neighbor_update(rivr_neighbor_table_t *tbl,
                                 uint32_t               node_id,
                                 int16_t                rssi_dbm,
                                 int8_t                 snr_db,
                                 uint8_t                hop_count,
                                 uint16_t               seq,
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
