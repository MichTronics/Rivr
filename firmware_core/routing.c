/**
 * @file  routing.c
 * @brief RIVR mesh routing — dedupe cache, TTL management, neighbour table,
 *        Phase-A flood hardening, Phase-D route-discovery helpers.
 */

#include "routing.h"
#include "route_cache.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "rivr_metrics.h"

/* ── Module-global state (BSS) ───────────────────────────────────────────── */
static dedupe_cache_t   g_dedupe;
static forward_budget_t g_fwdbudget;

/* ── dedupe cache ────────────────────────────────────────────────────────── */

void routing_dedupe_init(dedupe_cache_t *cache)
{
    memset(cache, 0, sizeof(*cache));
}

bool routing_dedupe_check(dedupe_cache_t *cache,
                          uint32_t        src_id,
                          uint16_t        pkt_id,
                          uint32_t        now_ms)
{
    if (!cache) return true;   /* safe default: accept */

    /* Scan entire ring for an unexpired match */
    for (uint8_t i = 0; i < DEDUPE_CACHE_SIZE; i++) {
        dedupe_entry_t *e = &cache->entries[i];

        if (e->src_id == 0) continue;   /* empty slot */

        /* Expire entries older than DEDUPE_EXPIRY_MS */
        uint32_t age = now_ms - e->seen_at_ms;
        if (age > DEDUPE_EXPIRY_MS) {
            memset(e, 0, sizeof(*e));
            continue;
        }

        if (e->src_id == src_id && e->pkt_id == pkt_id) {
            return false;   /* duplicate — suppress */
        }
    }

    /* Not seen before — insert at head (evicts oldest slot) */
    dedupe_entry_t *slot = &cache->entries[cache->head];
    slot->src_id     = src_id;
    slot->pkt_id     = pkt_id;
    slot->seen_at_ms = now_ms;
    cache->head      = (uint8_t)((cache->head + 1u) & (DEDUPE_CACHE_SIZE - 1u));

    return true;   /* new packet, accept */
}

bool routing_should_forward(dedupe_cache_t *cache,
                            rivr_pkt_hdr_t *pkt,
                            uint32_t        now_ms)
{
    if (!cache || !pkt) return false;

    /* Step 1: duplicate check */
    if (!routing_dedupe_check(cache, pkt->src_id, pkt->pkt_id, now_ms)) {
        return false;   /* duplicate */
    }

    /* Step 2: TTL check — if TTL is already 0 or 1, dropping after decrement */
    if (pkt->ttl == 0) {
        return false;   /* TTL already exhausted */
    }

    /* Decrement TTL, increment hop, mark as relayed */
    pkt->ttl  -= 1u;
    pkt->hop  += 1u;
    pkt->flags = (uint8_t)(pkt->flags | PKT_FLAG_RELAY);

    /* Forward only if TTL is still > 0 after decrement */
    return (pkt->ttl > 0);
}

/* ── neighbour table ─────────────────────────────────────────────────────── */

/* ── link score helper (shared by routing_neighbor_link_score & neighbor_update) ── *
 * rssi_part: clamp(rssi + 140, 0, 80)  — 0 pts at -140 dBm, 80 pts at -60 dBm  *
 * snr_part : clamp(snr  +  10, 0, 20)  — 0 pts at -10 dB,  20 pts at >=+10 dB  *
 * base = rssi_part + snr_part → 0..100                                           */
static uint8_t neighbor_compute_base(int8_t rssi_dbm, int8_t snr_db)
{
    int32_t r = (int32_t)rssi_dbm + 140;
    if (r < 0)  r = 0;
    if (r > 80) r = 80;
    int32_t s = (int32_t)snr_db + 10;
    if (s < 0)  s = 0;
    if (s > 20) s = 20;
    return (uint8_t)(r + s);
}

uint8_t routing_neighbor_link_score(const neighbor_entry_t *n, uint32_t now_ms)
{
    if (!n || n->node_id == 0) return 0u;
    uint32_t age = now_ms - n->last_seen_ms;
    if (age >= NEIGHBOR_EXPIRY_MS) return 0u;
    uint8_t base = neighbor_compute_base(n->rssi_dbm, n->last_snr_db);
    /* Linear decay: full score at age=0, 0 at age=NEIGHBOR_EXPIRY_MS       */
    return (uint8_t)((uint32_t)base * (NEIGHBOR_EXPIRY_MS - age)
                     / NEIGHBOR_EXPIRY_MS);
}

void routing_neighbor_print(const neighbor_table_t *tbl, uint32_t now_ms)
{
    if (!tbl) return;
    printf("%-12s %-12s %4s %8s %7s %5s %6s %6s\r\n",
           "NodeID", "Callsign", "Hops", "RSSI(dB)", "SNR(dB)",
           "Score", "Age(s)", "rx_ok");
    uint8_t shown = 0u;
    for (uint8_t i = 0u; i < NEIGHBOR_TABLE_SIZE; i++) {
        const neighbor_entry_t *n = &tbl->entries[i];
        if (n->node_id == 0) continue;
        uint32_t age_ms = now_ms - n->last_seen_ms;
        if (age_ms > NEIGHBOR_EXPIRY_MS) continue;
        uint8_t score = routing_neighbor_link_score(n, now_ms);
        printf("0x%08lX  %-12s %4u %8d %7d %5u %6lu %6lu\r\n",
               (unsigned long)n->node_id,
               n->callsign[0] ? n->callsign : "-",
               (unsigned)n->hop_count,
               (int)n->rssi_dbm,
               (int)n->last_snr_db,
               (unsigned)score,
               (unsigned long)(age_ms / 1000u),
               (unsigned long)n->rx_ok);
        shown++;
    }
    if (shown == 0u) printf("  (no live neighbours)\r\n");
}

void routing_neighbor_init(neighbor_table_t *tbl)
{
    memset(tbl, 0, sizeof(*tbl));
}

void routing_neighbor_update(neighbor_table_t       *tbl,
                             const rivr_pkt_hdr_t   *pkt,
                             int8_t                  rssi_dbm,
                             int8_t                  snr_db,
                             uint32_t                now_ms)
{
    if (!tbl || !pkt || pkt->src_id == 0) return;

    /* Search for existing entry */
    int8_t oldest_idx   = -1;
    uint32_t oldest_age = 0;

    for (uint8_t i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        neighbor_entry_t *n = &tbl->entries[i];

        if (n->node_id == 0) {
            /* Empty slot — use it immediately */
            n->node_id      = pkt->src_id;
            n->last_seen_ms = now_ms;
            n->rssi_dbm     = rssi_dbm;
            n->last_snr_db  = snr_db;
            n->hop_count    = (uint8_t)(pkt->hop + 1u);
            n->rx_ok        = 1u;
            if (tbl->count < NEIGHBOR_TABLE_SIZE) { tbl->count++; }
            return;
        }

        if (n->node_id == pkt->src_id) {
            /* Update existing entry — EWMA for RSSI and SNR (α=1/8) to
             * suppress short-lived spikes and prevent route oscillation. */
            n->last_seen_ms = now_ms;
            n->rssi_dbm     = (int8_t)(((int16_t)n->rssi_dbm * 7
                                         + (int16_t)rssi_dbm) / 8);
            n->last_snr_db  = (int8_t)(((int16_t)n->last_snr_db * 7
                                         + (int16_t)snr_db) / 8);
            n->rx_ok++;
            /* Only update hop_count if this path is shorter */
            uint8_t new_hops = (uint8_t)(pkt->hop + 1u);
            if (new_hops < n->hop_count) { n->hop_count = new_hops; }
            return;
        }

        /* Track oldest for eviction */
        uint32_t age = now_ms - n->last_seen_ms;
        if (oldest_idx < 0 || age > oldest_age) {
            oldest_idx = (int8_t)i;
            oldest_age = age;
        }
    }

    /* Table full — evict the oldest or most-expired entry */
    if (oldest_idx >= 0) {
        neighbor_entry_t *n = &tbl->entries[oldest_idx];
        n->node_id      = pkt->src_id;
        n->last_seen_ms = now_ms;
        n->rssi_dbm     = rssi_dbm;
        n->last_snr_db  = snr_db;
        n->hop_count    = (uint8_t)(pkt->hop + 1u);
        n->rx_ok        = 1u;
    }
}

uint8_t routing_neighbor_count(const neighbor_table_t *tbl, uint32_t now_ms)
{
    if (!tbl) return 0;
    uint8_t alive = 0;
    for (uint8_t i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        const neighbor_entry_t *n = &tbl->entries[i];
        if (n->node_id == 0) continue;
        uint32_t age = now_ms - n->last_seen_ms;
        if (age <= NEIGHBOR_EXPIRY_MS) { alive++; }
    }
    return alive;
}

const neighbor_entry_t *routing_neighbor_get(const neighbor_table_t *tbl,
                                              uint8_t idx)
{
    if (!tbl || idx >= NEIGHBOR_TABLE_SIZE) return NULL;
    const neighbor_entry_t *n = &tbl->entries[idx];
    return (n->node_id != 0) ? n : NULL;
}

void routing_neighbor_set_callsign(neighbor_table_t *tbl,
                                   uint32_t          node_id,
                                   const char       *callsign)
{
    if (!tbl || node_id == 0 || !callsign) return;
    for (uint8_t i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        neighbor_entry_t *n = &tbl->entries[i];
        if (n->node_id == node_id) {
            /* Copy at most 11 chars + NUL terminator */
            uint8_t j = 0;
            while (j < (uint8_t)(sizeof(n->callsign) - 1u) && callsign[j] != '\0') {
                n->callsign[j] = callsign[j];
                j++;
            }
            n->callsign[j] = '\0';
            return;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * PHASE A — Flood hardening
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Global state accessors ──────────────────────────────────────────────── */

void routing_init(void)
{
    routing_dedupe_init(&g_dedupe);
    routing_fwdbudget_init(&g_fwdbudget);
}

dedupe_cache_t *routing_get_dedupe(void)    { return &g_dedupe; }
forward_budget_t *routing_get_fwdbudget(void) { return &g_fwdbudget; }

/* ── Forward budget ──────────────────────────────────────────────────────── */

void routing_fwdbudget_init(forward_budget_t *fb)
{
    if (!fb) return;
    memset(fb, 0, sizeof(*fb));
    fb->max_fwd_count    = FWDBUDGET_MAX_FWD;
    fb->max_air_us       = FWDBUDGET_MAX_AIR_US;
    fb->max_hour_air_us  = FWDBUDGET_MAX_HOUR_AIR_US;
}

static void fwdbudget_roll_window(forward_budget_t *fb, uint32_t now_ms)
{
    /* Per-minute window */
    uint32_t age = now_ms - fb->window_start_ms;
    if (age >= FWDBUDGET_WINDOW_MS) {
        memset(fb->fwd_count,           0, sizeof(fb->fwd_count));
        memset(fb->fwd_air_us,          0, sizeof(fb->fwd_air_us));
        memset(fb->fwd_drop_count,      0, sizeof(fb->fwd_drop_count));
        memset(fb->tx_originated_count, 0, sizeof(fb->tx_originated_count));
        fb->window_start_ms = now_ms;
    }

    /* Per-hour window (separate reset) */
    uint32_t hour_age = now_ms - fb->hour_window_start_ms;
    if (hour_age >= FWDBUDGET_HOUR_WINDOW_MS) {
        fb->hour_total_air_us    = 0u;
        fb->hour_window_start_ms = now_ms;
    }
}

bool routing_fwdbudget_check(forward_budget_t *fb,
                              uint8_t           pkt_type,
                              uint32_t          toa_us,
                              uint32_t          now_ms)
{
    if (!fb) return true;
    if (pkt_type >= FWDBUDGET_PKT_TYPES) return true;

    fwdbudget_roll_window(fb, now_ms);

    /* Per-minute per-type caps */
    if (fb->fwd_count[pkt_type] >= fb->max_fwd_count)         return false;
    if (fb->fwd_air_us[pkt_type] + toa_us > fb->max_air_us)   return false;

    /* Per-hour total airtime cap */
    if (fb->hour_total_air_us + toa_us > fb->max_hour_air_us) return false;

    return true;
}

void routing_fwdbudget_record(forward_budget_t *fb,
                               uint8_t           pkt_type,
                               uint32_t          toa_us,
                               uint32_t          now_ms)
{
    if (!fb || pkt_type >= FWDBUDGET_PKT_TYPES) return;
    fwdbudget_roll_window(fb, now_ms);
    fb->fwd_count[pkt_type]  += 1u;
    fb->fwd_air_us[pkt_type] += toa_us;
    fb->hour_total_air_us    += toa_us;
}

/* ── Strict flood forward ────────────────────────────────────────────────── */

rivr_fwd_result_t routing_flood_forward(dedupe_cache_t   *cache,
                                         forward_budget_t *fb,
                                         rivr_pkt_hdr_t   *pkt,
                                         uint32_t          my_id,
                                         uint32_t          toa_us,
                                         uint32_t          now_ms)
{
    /* Step 1 — Deduplicate (keyed on src_id + pkt_id, NOT application seq) */
    if (!routing_dedupe_check(cache, pkt->src_id, pkt->pkt_id, now_ms)) {
        g_rivr_metrics.rx_dedupe_drop++;
        return RIVR_FWD_DROP_DEDUPE;
    }

    /* Step 2 — TTL must be > 0 on arrival */
    if (pkt->ttl == 0u) {
        g_rivr_metrics.rx_ttl_drop++;
        return RIVR_FWD_DROP_TTL;
    }

    /* Step 3 — Loop-guard check (skip if my_id == 0, e.g. replay/fuzz) */
    uint8_t my_h = 0u;
    if (my_id != 0u) {
        my_h = routing_loop_guard_hash(my_id);
        if ((pkt->loop_guard & my_h) == my_h) {
            /* This node's fingerprint is already in the guard byte — the
             * packet has looped back here (possibly with a mutated seq that
             * defeated the dedupe cache).  Drop and count. */
            g_rivr_metrics.loop_detect_drop++;
            g_rivr_metrics.loop_detect_drop_total++;
            return RIVR_FWD_DROP_LOOP;
        }
    }

    /* Step 4 — Safety budget */
    if (fb && !routing_fwdbudget_check(fb, pkt->pkt_type, toa_us, now_ms)) {
        if (pkt->pkt_type < FWDBUDGET_PKT_TYPES) {
            fb->fwd_drop_count[pkt->pkt_type] += 1u;
        }
        fb->total_fwd_drops += 1u;
        return RIVR_FWD_DROP_BUDGET;
    }

    /* Step 5 — Mutate header for relay */
    pkt->ttl        -= 1u;
    pkt->hop        += 1u;
    pkt->flags       = (uint8_t)(pkt->flags | PKT_FLAG_RELAY);
    if (my_id != 0u) {
        pkt->loop_guard = (uint8_t)(pkt->loop_guard | my_h);
    }

    /* Record against budget */
    if (fb) {
        routing_fwdbudget_record(fb, pkt->pkt_type, toa_us, now_ms);
    }

    return RIVR_FWD_FORWARD;
}

/* ── Jitter helper ───────────────────────────────────────────────────────── */

uint16_t routing_jitter_ticks(uint32_t src_id, uint16_t pkt_id, uint16_t max_j)
{
    if (max_j == 0u) return 0u;

    /* Knuth multiplicative hash then xorshift32 */
    uint32_t x = src_id ^ ((uint32_t)pkt_id * 2654435761u);
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x <<  5u;

    return (uint16_t)(x % (uint32_t)(max_j + 1u));
}

uint32_t routing_forward_delay_ms(uint32_t src_id, uint16_t pkt_id, uint8_t pkt_type)
{
#if FORWARD_JITTER_MAX_MS == 0
    (void)src_id; (void)pkt_id; (void)pkt_type;
    return 0u;
#else
    /* Seed uses (src_id, pkt_id, pkt_type): different injections (retransmits,
     * fallback floods) of the same logical message get independent jitter
     * because they carry distinct pkt_ids.  pkt_type adds type-aware spread
     * so different frame types from the same source don't collide. */
    uint32_t x = src_id ^ (uint32_t)pkt_id ^ ((uint32_t)pkt_type << 24u);
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x <<  5u;
    return x % ((uint32_t)FORWARD_JITTER_MAX_MS + 1u);
#endif
}

/* ════════════════════════════════════════════════════════════════════════════
 * PHASE D — Control-plane packets
 * ══════════════════════════════════════════════════════════════════════════ */

int routing_build_route_req(uint32_t  my_id,
                             uint32_t  target_id,
                             uint16_t  seq,
                             uint16_t  pkt_id,
                             uint8_t  *out_buf,
                             uint8_t   out_cap)
{
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_ROUTE_REQ;
    hdr.flags       = 0u;
    hdr.ttl         = ROUTE_REQ_TTL;
    hdr.hop         = 0u;
    hdr.net_id      = 0u;
    hdr.src_id      = my_id;
    hdr.dst_id      = target_id;   /* dst = target we seek; broadcast by TTL */
    hdr.seq         = seq;
    hdr.pkt_id      = pkt_id;
    hdr.payload_len = 0u;
    return protocol_encode(&hdr, NULL, 0u, out_buf, out_cap);
}

int routing_build_route_rpl(uint32_t  my_id,
                             uint32_t  requester_id,
                             uint32_t  target_id,
                             uint32_t  next_hop,
                             uint8_t   hop_count,
                             uint16_t  seq,
                             uint16_t  pkt_id,
                             uint8_t  *out_buf,
                             uint8_t   out_cap)
{
    /*
     * ROUTE_RPL uses directed-flood semantics (like AODV RREP):
     *   • dst_id  = requester_id  — so the requester recognises it
     *   • ttl    = ROUTE_REQ_TTL  — floods through the mesh just like ROUTE_REQ
     *   • Any overhearer may learn the route as a side-effect; only the
     *     requester (dst_id match) must drain its pending queue.
     *
     * Payload layout (ROUTE_RPL_PAYLOAD_LEN = 9 bytes, all little-endian):
     *   [0–3]  target_id  u32 LE — the destination the ROUTE_REQ was seeking
     *   [4–7]  next_hop   u32 LE — replier's next hop toward target_id
     *   [8]    hop_count  u8     — hops from replier to target_id
     *
     * Requester reconstructs total hops as:
     *   total = hop_count + pkt_hdr.hop + 1
     */
    uint8_t pl[ROUTE_RPL_PAYLOAD_LEN];
    pl[0] = (uint8_t)(target_id      & 0xFFu);
    pl[1] = (uint8_t)((target_id >>  8u) & 0xFFu);
    pl[2] = (uint8_t)((target_id >> 16u) & 0xFFu);
    pl[3] = (uint8_t)((target_id >> 24u) & 0xFFu);
    pl[4] = (uint8_t)(next_hop       & 0xFFu);
    pl[5] = (uint8_t)((next_hop  >>  8u) & 0xFFu);
    pl[6] = (uint8_t)((next_hop  >> 16u) & 0xFFu);
    pl[7] = (uint8_t)((next_hop  >> 24u) & 0xFFu);
    pl[8] = hop_count;

    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_ROUTE_RPL;
    hdr.flags       = 0u;
    hdr.ttl         = ROUTE_REQ_TTL;   /* flood reply with same TTL */
    hdr.hop         = 0u;
    hdr.net_id      = 0u;
    hdr.src_id      = my_id;
    hdr.dst_id      = requester_id;
    hdr.seq         = seq;
    hdr.pkt_id      = pkt_id;
    hdr.payload_len = ROUTE_RPL_PAYLOAD_LEN;
    return protocol_encode(&hdr, pl, ROUTE_RPL_PAYLOAD_LEN, out_buf, out_cap);
}

bool routing_should_reply_route_req(const rivr_pkt_hdr_t *pkt,
                                     uint32_t              my_id,
                                     route_cache_t        *cache,
                                     uint32_t              now_ms)
{
    if (!pkt || pkt->pkt_type != PKT_ROUTE_REQ) return false;

    /* Case 1: We ARE the requested destination. */
    if (pkt->dst_id == my_id) return true;

    /* Case 2: We have a live, eligible (non-pending, sufficient quality,
     * limited hop depth) cached route for the target.
     * route_cache_can_reply_for_dst() encapsulates all reply-eligibility
     * gates: expiry, PENDING flag, metric threshold, max hop count. */
    if (!cache) return false;
    return route_cache_can_reply_for_dst(cache, pkt->dst_id, now_ms);
}
