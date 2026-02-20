/**
 * @file  routing.c
 * @brief RIVR mesh routing — dedupe cache, TTL management, neighbour table,
 *        Phase-A flood hardening, Phase-D route-discovery helpers.
 */

#include "routing.h"
#include "protocol.h"
#include <string.h>

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
                          uint32_t        seq,
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

        if (e->src_id == src_id && e->seq == seq) {
            return false;   /* duplicate — suppress */
        }
    }

    /* Not seen before — insert at head (evicts oldest slot) */
    dedupe_entry_t *slot = &cache->entries[cache->head];
    slot->src_id     = src_id;
    slot->seq        = seq;
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
    if (!routing_dedupe_check(cache, pkt->src_id, pkt->seq, now_ms)) {
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

void routing_neighbor_init(neighbor_table_t *tbl)
{
    memset(tbl, 0, sizeof(*tbl));
}

void routing_neighbor_update(neighbor_table_t       *tbl,
                             const rivr_pkt_hdr_t   *pkt,
                             int8_t                  rssi_dbm,
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
            n->hop_count    = (uint8_t)(pkt->hop + 1u);
            if (tbl->count < NEIGHBOR_TABLE_SIZE) { tbl->count++; }
            return;
        }

        if (n->node_id == pkt->src_id) {
            /* Update existing entry */
            n->last_seen_ms = now_ms;
            n->rssi_dbm     = rssi_dbm;
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
        n->hop_count    = (uint8_t)(pkt->hop + 1u);
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
        memset(fb->fwd_count,      0, sizeof(fb->fwd_count));
        memset(fb->fwd_air_us,     0, sizeof(fb->fwd_air_us));
        memset(fb->fwd_drop_count, 0, sizeof(fb->fwd_drop_count));
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
                                         uint32_t          toa_us,
                                         uint32_t          now_ms)
{
    /* Step 1 — Deduplicate */
    if (!routing_dedupe_check(cache, pkt->src_id, pkt->seq, now_ms)) {
        return RIVR_FWD_DROP_DEDUPE;
    }

    /* Step 2 — TTL must be > 0 on arrival */
    if (pkt->ttl == 0u) {
        return RIVR_FWD_DROP_TTL;
    }

    /* Step 3 — Safety budget */
    if (fb && !routing_fwdbudget_check(fb, pkt->pkt_type, toa_us, now_ms)) {
        if (pkt->pkt_type < FWDBUDGET_PKT_TYPES) {
            fb->fwd_drop_count[pkt->pkt_type] += 1u;
        }
        fb->total_fwd_drops += 1u;
        return RIVR_FWD_DROP_BUDGET;
    }

    /* Step 4 — Mutate header for relay */
    pkt->ttl  -= 1u;
    pkt->hop  += 1u;
    pkt->flags = (uint8_t)(pkt->flags | PKT_FLAG_RELAY);

    /* Record against budget */
    if (fb) {
        routing_fwdbudget_record(fb, pkt->pkt_type, toa_us, now_ms);
    }

    return RIVR_FWD_FORWARD;
}

/* ── Jitter helper ───────────────────────────────────────────────────────── */

uint16_t routing_jitter_ticks(uint32_t src_id, uint32_t seq, uint16_t max_j)
{
    if (max_j == 0u) return 0u;

    /* Knuth multiplicative hash then xorshift32 */
    uint32_t x = src_id ^ (seq * 2654435761u);
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x <<  5u;

    return (uint16_t)(x % (uint32_t)(max_j + 1u));
}

uint32_t routing_forward_delay_ms(uint32_t src_id, uint32_t seq, uint8_t pkt_type)
{
#if FORWARD_JITTER_MAX_MS == 0
    (void)src_id; (void)seq; (void)pkt_type;
    return 0u;
#else
    /* Seed includes pkt_type so different frame types spread independently
     * even when (src_id, seq) is the same (e.g. rapid back-to-back frames). */
    uint32_t x = src_id ^ seq ^ ((uint32_t)pkt_type << 24u);
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
                             uint32_t  seq,
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
    hdr.payload_len = 0u;
    return protocol_encode(&hdr, NULL, 0u, out_buf, out_cap);
}

int routing_build_route_rpl(uint32_t  my_id,
                             uint32_t  requester_id,
                             uint32_t  target_id,
                             uint32_t  next_hop,
                             uint8_t   hop_count,
                             uint32_t  seq,
                             uint8_t  *out_buf,
                             uint8_t   out_cap)
{
    /*
     * Payload (9 bytes, all little-endian):
     *   [0–3]  target_id  u32 LE
     *   [4–7]  next_hop   u32 LE
     *   [8]    hop_count  u8
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
    hdr.payload_len = ROUTE_RPL_PAYLOAD_LEN;
    return protocol_encode(&hdr, pl, ROUTE_RPL_PAYLOAD_LEN, out_buf, out_cap);
}

bool routing_should_reply_route_req(const rivr_pkt_hdr_t *pkt, uint32_t my_id)
{
    if (!pkt || pkt->pkt_type != PKT_ROUTE_REQ) return false;
    /* Reply if WE are the requested destination */
    return (pkt->dst_id == my_id);
}
