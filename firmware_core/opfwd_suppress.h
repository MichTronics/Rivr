/**
 * @file  opfwd_suppress.h
 * @brief Opportunistic forward-suppression table (Phase 4).
 *
 * Small BSS ring that records (src_id, pkt_id) pairs overheard being
 * relayed by a neighbor while our own queued copy is still in the jitter
 * window.  The tx_drain_loop() checks this table before transmitting any
 * relay frame and skips the TX if the pair is present, saving airtime and
 * reducing channel collisions on dense meshes.
 *
 * MECHANISM
 * ─────────
 *  RX path (rivr_sources.c):
 *    When routing_flood_forward() returns RIVR_FWD_DROP_DEDUPE AND the
 *    incoming frame has PKT_FLAG_RELAY set, a neighbor has already relayed
 *    this packet.  Call opfwd_suppress_add() to record (src_id, pkt_id).
 *
 *  TX path (main.c tx_drain_loop):
 *    Before transmitting a relay frame (PKT_FLAG_RELAY set), call
 *    opfwd_suppress_check().  If the entry is present and unexpired,
 *    skip the TX and increment flood_fwd_cancelled_opport_total.
 *
 * CONSTRAINTS
 * ───────────
 *  • Zero CPU/RAM overhead when RIVR_FEATURE_OPPORTUNISTIC_FWD = 0 — all
 *    call sites are inside #if guards; this header still compiles cleanly.
 *  • No heap: all state is BSS (zero-initialise = all empty).
 *  • All functions are static-inline; no .c compilation unit needed.
 *  • Expiry window: FORWARD_JITTER_MAX_MS (200 ms) + 100 ms safety margin
 *    = OPFWD_SUPPRESS_EXPIRY_MS (300 ms).
 */

#ifndef RIVR_OPFWD_SUPPRESS_H
#define RIVR_OPFWD_SUPPRESS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ────────────────────────────────────────────────────────── */

/** Number of suppression-table slots (16 covers ≥ 4 active neighbors × 4 in-flight). */
#define OPFWD_SUPPRESS_SIZE        16u

/**
 * Entry lifetime in milliseconds.
 * FORWARD_JITTER_MAX_MS = 200 ms; add 100 ms safety margin for ISR/TX latency.
 * An entry that survives beyond 300 ms is irrelevant — the jitter window
 * has long expired and any queued copy has already been transmitted.
 */
#define OPFWD_SUPPRESS_EXPIRY_MS   300u

/* ── Types ────────────────────────────────────────────────────────────────── */

/** One entry: an (src_id, pkt_id) pair whose relay has been overheard. */
typedef struct {
    uint32_t src_id;       /**< Originating source node ID (0 = empty slot)  */
    uint16_t pkt_id;       /**< Per-injection forward identity               */
    uint16_t _pad;         /**< Alignment padding                            */
    uint32_t expires_ms;   /**< tb_millis() value at which entry becomes stale */
} opfwd_entry_t;           /* 12 bytes; 16 entries = 192 bytes BSS            */

/** Complete suppression table (BSS-safe: zero-init = all empty). */
typedef struct {
    opfwd_entry_t entries[OPFWD_SUPPRESS_SIZE];
    uint8_t       next_write;  /**< Round-robin write index (wraps at SIZE)   */
    uint8_t       _pad[3];
} opfwd_suppress_t;

/* ── API (static-inline — no .c file needed) ─────────────────────────────── */

/**
 * @brief Record that a neighbor has relayed (src_id, pkt_id).
 *
 * If the pair is already present, its expiry is refreshed.  Otherwise the
 * entry is written into the round-robin slot at next_write, overwriting the
 * oldest entry unconditionally — the table never stalls.
 *
 * Call from the DEDUPE-drop branch of the RX path when PKT_FLAG_RELAY is set.
 *
 * @param tbl     Suppression table.
 * @param src_id  Originating node ID from the overheard relay frame.
 * @param pkt_id  Per-injection pkt_id from the overheard relay frame.
 * @param now_ms  Current tb_millis().
 */
static inline void opfwd_suppress_add(opfwd_suppress_t *tbl,
                                       uint32_t          src_id,
                                       uint16_t          pkt_id,
                                       uint32_t          now_ms)
{
    if (!tbl || src_id == 0u) return;

    /* Refresh expiry if already present (avoids double-write). */
    for (uint8_t i = 0u; i < OPFWD_SUPPRESS_SIZE; i++) {
        opfwd_entry_t *e = &tbl->entries[i];
        if (e->src_id == src_id && e->pkt_id == pkt_id) {
            e->expires_ms = now_ms + OPFWD_SUPPRESS_EXPIRY_MS;
            return;
        }
    }

    /* Not found: overwrite the round-robin slot. */
    opfwd_entry_t *slot = &tbl->entries[tbl->next_write];
    slot->src_id     = src_id;
    slot->pkt_id     = pkt_id;
    slot->_pad       = 0u;
    slot->expires_ms = now_ms + OPFWD_SUPPRESS_EXPIRY_MS;
    tbl->next_write  = (uint8_t)((tbl->next_write + 1u) % OPFWD_SUPPRESS_SIZE);
}

/**
 * @brief Check whether (src_id, pkt_id) is in the suppression window.
 *
 * Returns true only when the entry exists AND has not yet expired.
 * Expired entries are treated as absent (lazy expiry — no separate purge).
 *
 * Call from tx_drain_loop() before transmitting a relay frame.
 *
 * @param tbl     Suppression table (const — read only).
 * @param src_id  Source node ID of the relay frame about to be transmitted.
 * @param pkt_id  pkt_id of the relay frame about to be transmitted.
 * @param now_ms  Current tb_millis().
 * @return true  → overheard relayed, still fresh; suppress our TX copy.
 * @return false → not found or expired; transmit normally.
 */
static inline bool opfwd_suppress_check(const opfwd_suppress_t *tbl,
                                         uint32_t                src_id,
                                         uint16_t                pkt_id,
                                         uint32_t                now_ms)
{
    if (!tbl || src_id == 0u) return false;

    for (uint8_t i = 0u; i < OPFWD_SUPPRESS_SIZE; i++) {
        const opfwd_entry_t *e = &tbl->entries[i];
        if (e->src_id == src_id && e->pkt_id == pkt_id
                && (int32_t)(e->expires_ms - now_ms) > 0) {
            return true;
        }
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* RIVR_OPFWD_SUPPRESS_H */
