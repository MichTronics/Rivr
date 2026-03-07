/**
 * @file  neighbor_table.c
 * @brief Standalone neighbor-table subsystem for Rivr firmware.
 *
 * See neighbor_table.h for the public API and design notes.
 */

#include "neighbor_table.h"

#include <string.h>  /* memset, memcpy */
#include <stddef.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

/** Saturating integer clamp (inclusive). */
static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**
 * Find the slot index for @p node_id.
 * Returns the slot index on match, or -1 if not present.
 */
static int find_slot(const rivr_neighbor_table_t *tbl, uint32_t node_id)
{
    for (uint8_t i = 0u; i < tbl->count; i++) {
        if (tbl->entries[i].neighbor_id == node_id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Find the index of the entry with the smallest last_seen_ms (oldest).
 * Returns 0 if the table is empty.
 */
static uint8_t oldest_slot(const rivr_neighbor_table_t *tbl)
{
    uint8_t oldest = 0u;
    uint32_t min_ts = tbl->entries[0].last_seen_ms;
    for (uint8_t i = 1u; i < tbl->count; i++) {
        if (tbl->entries[i].last_seen_ms < min_ts) {
            min_ts = tbl->entries[i].last_seen_ms;
            oldest = i;
        }
    }
    return oldest;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void neighbor_table_init(rivr_neighbor_table_t *tbl)
{
    if (!tbl) return;
    memset(tbl, 0, sizeof(*tbl));
}

rivr_neighbor_t *neighbor_update(rivr_neighbor_table_t *tbl,
                                 uint32_t               node_id,
                                 int16_t                rssi_dbm,
                                 int8_t                 snr_db,
                                 uint8_t                hop_count,
                                 uint16_t               seq,
                                 uint32_t               now_ms)
{
    if (!tbl || node_id == 0u) return NULL;

    /* ── Locate or allocate slot ──────────────────────────────────────────── */
    int idx = find_slot(tbl, node_id);
    rivr_neighbor_t *n;

    if (idx >= 0) {
        /* Existing entry — update metrics. */
        n = &tbl->entries[idx];

        /* EWMA update for RSSI and SNR (α = 1/8). */
        n->rssi_avg = (int16_t)(((int32_t)n->rssi_avg * 7 + (int32_t)rssi_dbm) / 8);
        n->snr_avg  = (int8_t) (((int32_t)n->snr_avg  * 7 + (int32_t)snr_db)   / 8);

        /* Loss-rate estimation via sequence-number gaps.
         *
         * Compute how many sequence numbers were skipped since last seen.
         * The subtraction is unsigned (uint16_t wraps correctly) so it
         * handles the 0xFFFF→0x0000 rollover transparently.
         *
         * Gap:  0 = no missed frames (consecutive seq).
         *       1 = one missed frame.
         *      >1 = multiple missed frames.
         *
         * The first receive of any node seeds last_seq so this branch only
         * executes from the second observation onward.
         */
        if (n->rx_ok > 0u) {
            uint16_t gap = (uint16_t)(seq - n->last_seq - 1u);

            /* Cap the gap to avoid over-penalising after a node reboot or a
             * long silence (the expiry mechanism handles that separately). */
            if (gap > NTABLE_LOSS_MAX_GAP) {
                gap = NTABLE_LOSS_MAX_GAP;
            }

            /* Drive EWMA toward 100 (=loss) for each missed slot. */
            for (uint16_t m = 0u; m < gap; m++) {
                n->loss_rate = (uint8_t)(((uint32_t)n->loss_rate * 7u + 100u) / 8u);
            }
            /* Drive EWMA toward 0 (=no loss) for the frame just received. */
            n->loss_rate = (uint8_t)(((uint32_t)n->loss_rate * 7u + 0u) / 8u);
        }

        /* Keep track of the minimum observed hop distance. */
        if (hop_count < n->hop_count) {
            n->hop_count = hop_count;
        }
    } else {
        /* New peer — allocate slot. */
        uint8_t slot;
        if (tbl->count < NTABLE_SIZE) {
            /* Free slot available. */
            slot = tbl->count;
            tbl->count++;
        } else {
            /* Table full — evict oldest entry. */
            slot = oldest_slot(tbl);
        }

        n = &tbl->entries[slot];
        memset(n, 0, sizeof(*n));

        /* Seed the EWMA fields with the first sample so the entry is
         * immediately useful rather than starting from zero. */
        n->neighbor_id  = node_id;
        n->rssi_avg     = rssi_dbm;
        n->snr_avg      = snr_db;
        n->loss_rate    = 0u;  /* optimistic seed — no loss history yet */
        n->hop_count    = hop_count;
        /* last_seq seeded below; loss-rate gap not applied on first receive */
    }

    /* ── Fields updated on every observation ─────────────────────────────── */
    n->last_seq     = seq;
    n->last_seen_ms = now_ms;
    n->rx_ok++;

    /* Refresh flags. */
    n->flags &= (uint8_t)~NTABLE_FLAG_STALE;   /* clear stale; will be re-set lazily */
    if (hop_count == 0u) {
        n->flags |= NTABLE_FLAG_DIRECT;
    }

    return n;
}

const rivr_neighbor_t *neighbor_find(const rivr_neighbor_table_t *tbl,
                                     uint32_t                     node_id,
                                     uint32_t                     now_ms)
{
    if (!tbl || node_id == 0u) return NULL;

    for (uint8_t i = 0u; i < tbl->count; i++) {
        const rivr_neighbor_t *n = &tbl->entries[i];
        if (n->neighbor_id != node_id) continue;

        uint32_t age = now_ms - n->last_seen_ms;

        /* Expired — treat as absent. */
        if (age >= NTABLE_EXPIRY_MS) return NULL;

        /* Update STALE flag as a derived/cached property.
         * The cast-away of const is intentional: the flag is a pure
         * function of existing data and changes no observable metric. */
        rivr_neighbor_t *mutable_n = (rivr_neighbor_t *)(uintptr_t)n;
        if (age >= NTABLE_STALE_MS) {
            mutable_n->flags |= NTABLE_FLAG_STALE;
        } else {
            mutable_n->flags &= (uint8_t)~NTABLE_FLAG_STALE;
        }

        return n;
    }
    return NULL;
}

uint8_t neighbor_link_score(const rivr_neighbor_t *n, uint32_t now_ms)
{
    if (!n || n->neighbor_id == 0u) return 0u;

    uint32_t age = now_ms - n->last_seen_ms;
    if (age >= NTABLE_EXPIRY_MS) return 0u;

    /* Base quality: rssi_part (0..80) + snr_part (0..20) = 0..100 */
    int32_t rssi_part = clamp_i32((int32_t)n->rssi_avg + 140, 0, 80);
    int32_t snr_part  = clamp_i32((int32_t)n->snr_avg  +  10, 0, 20);
    uint32_t base     = (uint32_t)(rssi_part + snr_part);   /* 0..100 */

    /* Loss penalty: scale base by (100 − loss_rate)/100. */
    uint32_t after_loss = base * (uint32_t)(100u - n->loss_rate) / 100u;

    /* Linear age decay: score → 0 as age → NTABLE_EXPIRY_MS. */
    uint32_t score = after_loss * (NTABLE_EXPIRY_MS - age) / NTABLE_EXPIRY_MS;

    return (uint8_t)(score > 100u ? 100u : score);
}

const rivr_neighbor_t *neighbor_best(const rivr_neighbor_table_t *tbl,
                                     uint32_t                     now_ms)
{
    if (!tbl) return NULL;

    const rivr_neighbor_t *best  = NULL;
    uint8_t                best_score = 0u;

    for (uint8_t i = 0u; i < tbl->count; i++) {
        const rivr_neighbor_t *n = &tbl->entries[i];
        if (n->neighbor_id == 0u) continue;

        uint32_t age = now_ms - n->last_seen_ms;
        if (age >= NTABLE_EXPIRY_MS) continue;

        uint8_t sc = neighbor_link_score(n, now_ms);
        if (sc > best_score) {
            best_score = sc;
            best       = n;
        }
    }

    return best;
}

void neighbor_set_flag(rivr_neighbor_table_t *tbl,
                       uint32_t               node_id,
                       uint8_t                flag)
{
    if (!tbl || node_id == 0u) return;
    int idx = find_slot(tbl, node_id);
    if (idx >= 0) {
        tbl->entries[idx].flags |= flag;
    }
}

uint8_t neighbor_table_expire(rivr_neighbor_table_t *tbl, uint32_t now_ms)
{
    if (!tbl) return 0u;

    uint8_t removed = 0u;
    uint8_t i = 0u;

    while (i < tbl->count) {
        rivr_neighbor_t *n = &tbl->entries[i];
        uint32_t age = now_ms - n->last_seen_ms;

        if (age >= NTABLE_EXPIRY_MS) {
            /* Compact: overwrite with last entry, then shrink. */
            uint8_t last = tbl->count - 1u;
            if (i != last) {
                tbl->entries[i] = tbl->entries[last];
            }
            memset(&tbl->entries[last], 0, sizeof(rivr_neighbor_t));
            tbl->count--;
            removed++;
            /* Do NOT increment i — the swapped-in entry needs checking. */
        } else {
            i++;
        }
    }

    return removed;
}
