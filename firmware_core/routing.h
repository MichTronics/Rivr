/**
 * @file  routing.h
 * @brief RIVR mesh routing layer — duplicate suppression, TTL management,
 *        flood-forward hardening (Phase A), and hybrid unicast support (Phase D).
 *
 * PHASE A — Flood hardening:
 *  1. `routing_flood_forward()` — strict flood decision with explicit
 *     DEDUPE / TTL / BUDGET drop reasons.
 *  2. `routing_jitter_ticks()` — deterministic xorshift jitter in [0..J].
 *  3. `forward_budget_t` — per-pkt_type forwards/minute + airtime cap so
 *     the C layer enforces safety even if the RIVR policy misbehaves.
 *
 * PHASE D — Hybrid unicast hooks:
 *  4. `routing_build_route_req()` — encode a PKT_ROUTE_REQ broadcast.
 *  5. `routing_build_route_rpl()` — encode a PKT_ROUTE_RPL unicast reply.
 *  6. `routing_handle_route_req()` — react to an inbound ROUTE_REQ.
 *  7. `routing_handle_route_rpl()` — extract route info from ROUTE_RPL.
 *
 * GLOBAL STATE
 *  The module owns all state internally.  Call `routing_init()` once.
 *  `routing_get_dedupe()`, `routing_get_fwdbudget()` return pointers to the
 *  BSS-resident instances for use by the glue layer.
 *
 * All state is BSS-allocated — no heap, no dynamic memory.
 */

#ifndef RIVR_ROUTING_H
#define RIVR_ROUTING_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Dedupe cache ──────────────────────────────────────────────────────────── *
 *
 * A power-of-2 ring buffer (DEDUPE_CACHE_SIZE entries) that stores the most
 * recently seen (src_id, seq) pairs.  On each new packet:
 *   - Scan the ring for a matching entry → duplicate, suppress.
 *   - If not found → insert at `head`, advance head (oldest entry evicted).
 *
 * Expiry: entries older than DEDUPE_EXPIRY_MS are treated as not-seen so that
 * a legitimate retransmit after a long silence is accepted.
 * ────────────────────────────────────────────────────────────────────────── */

#define DEDUPE_CACHE_SIZE    32u    /**< Ring-buffer capacity (power of 2)   */
#define DEDUPE_EXPIRY_MS  60000u    /**< Entry expiry: 60 seconds            */

/** Single deduplicate-cache entry. */
typedef struct {
    uint32_t src_id;       /**< Source node ID (0 = empty slot)             */
    uint32_t seq;          /**< Sequence number                             */
    uint32_t seen_at_ms;   /**< Monotonic ms timestamp of first arrival     */
} dedupe_entry_t;

/** Complete duplicate-suppression state. */
typedef struct {
    dedupe_entry_t entries[DEDUPE_CACHE_SIZE];
    uint8_t        head;   /**< Next write position (ring head / oldest)    */
} dedupe_cache_t;

/* ── Neighbor table ─────────────────────────────────────────────────────── */

#define NEIGHBOR_TABLE_SIZE  16u   /**< Maximum tracked neighbours          */
#define NEIGHBOR_EXPIRY_MS   120000u /**< Evict neighbours unseen for 2 min  */

/** One entry in the neighbour table. */
typedef struct {
    uint32_t  node_id;        /**< Neighbour node ID (0 = empty slot)       */
    uint32_t  last_seen_ms;   /**< Monotonic ms of last received packet     */
    int8_t    rssi_dbm;       /**< Last observed RSSI                       */
    uint8_t   hop_count;      /**< Minimum hops to this neighbour (=hop+1)  */
} neighbor_entry_t;

/** Complete neighbour tracking state. */
typedef struct {
    neighbor_entry_t entries[NEIGHBOR_TABLE_SIZE];
    uint8_t          count;   /**< Number of active (non-expired) entries   */
} neighbor_table_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise a dedupe cache (zero all entries).
 */
void routing_dedupe_init(dedupe_cache_t *cache);

/**
 * @brief Check whether (src_id, seq) is genuinely new.
 *
 * If new: inserts the entry into the ring (evicting the oldest if full) and
 * returns true.
 * If already seen (and not yet expired): returns false.
 *
 * @param cache      Dedupe cache state.
 * @param src_id     Sender node ID.
 * @param seq        Packet sequence number.
 * @param now_ms     Current monotonic millisecond timestamp.
 *
 * @return true  → packet is new, inject / forward it.
 * @return false → duplicate, suppress it.
 */
bool routing_dedupe_check(dedupe_cache_t *cache,
                          uint32_t        src_id,
                          uint32_t        seq,
                          uint32_t        now_ms);

/**
 * @brief Decide whether a received packet should be re-forwarded.
 *
 * Performs (in order):
 *   1. Deduplicate check against @p cache.
 *   2. If new: decrement pkt->ttl, increment pkt->hop, set PKT_FLAG_RELAY.
 *   3. Returns true iff (new && ttl_after_decrement > 0).
 *
 * The caller should re-encode and re-inject the modified packet if this
 * function returns true.
 *
 * @param cache    Dedupe cache.
 * @param pkt      Decoded and mutable packet header (modified in place).
 * @param now_ms   Current monotonic millisecond timestamp.
 *
 * @return true  → forward the (modified) packet.
 * @return false → suppress (either duplicate or TTL exhausted).
 */
bool routing_should_forward(dedupe_cache_t *cache,
                            rivr_pkt_hdr_t *pkt,
                            uint32_t        now_ms);

/**
 * @brief Initialise the neighbour table (zero all entries).
 */
void routing_neighbor_init(neighbor_table_t *tbl);

/**
 * @brief Update the neighbour table with a freshly received packet header.
 *
 * Creates a new entry if the src_id is not yet known, otherwise refreshes the
 * existing entry.  Evicts the oldest entry when the table is full.
 *
 * @param tbl       Neighbour table.
 * @param pkt       Decoded packet header (src_id, hop used).
 * @param rssi_dbm  Observed RSSI of the received frame.
 * @param now_ms    Current monotonic millisecond timestamp.
 */
void routing_neighbor_update(neighbor_table_t       *tbl,
                             const rivr_pkt_hdr_t   *pkt,
                             int8_t                  rssi_dbm,
                             uint32_t                now_ms);

/**
 * @brief Return the number of alive (non-expired) neighbour entries.
 */
uint8_t routing_neighbor_count(const neighbor_table_t *tbl, uint32_t now_ms);

/* ════════════════════════════════════════════════════════════════════════════
 * PHASE A — Flood hardening
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Forward result ──────────────────────────────────────────────────────── */

typedef enum {
    RIVR_FWD_FORWARD,        /**< Packet accepted; pkt modified in place      */
    RIVR_FWD_DROP_DEDUPE,    /**< Duplicate — suppress                         */
    RIVR_FWD_DROP_TTL,       /**< TTL exhausted (already 0 on arrival)         */
    RIVR_FWD_DROP_BUDGET,    /**< Safety forward budget exceeded               */
} rivr_fwd_result_t;

/* ── Safety forward budget ───────────────────────────────────────────────── *
 *
 * Maintains independent counters per pkt_type (0–FWDBUDGET_PKT_TYPES-1).
 * Both a count-per-minute cap and an airtime-per-minute cap are enforced.
 * Counters reset each time now_ms crosses a new FWDBUDGET_WINDOW_MS boundary.
 * ─────────────────────────────────────────────────────────────────────────── */

#define FWDBUDGET_PKT_TYPES    8u           /**< Types 0–7                     */
#define FWDBUDGET_WINDOW_MS    60000u       /**< Rolling 1-minute window        */
#define FWDBUDGET_MAX_FWD      30u          /**< Max forwards / type / minute   */
#define FWDBUDGET_MAX_AIR_US   3000000u     /**< Max airtime / type / minute    */
                                            /**  (3 s = ~5% duty on SF9 BW125) */

/* Hour-level airtime cap (independent of the per-minute per-type caps). */
#define FWDBUDGET_HOUR_WINDOW_MS     3600000u  /**< 1-hour rolling window        */
#define FWDBUDGET_MAX_HOUR_AIR_US    36000000u /**< 36 s / hour ≈ 1% duty cycle  */

typedef struct {
    /* ── Per-minute, per-type counters ─────────────────────────────────── */
    uint32_t window_start_ms;                    /**< Minute-window start         */
    uint16_t fwd_count[FWDBUDGET_PKT_TYPES];     /**< Forwards this minute/type  */
    uint32_t fwd_air_us[FWDBUDGET_PKT_TYPES];    /**< Airtime this minute/type   */
    uint16_t max_fwd_count;   /**< Configurable cap (default FWDBUDGET_MAX_FWD) */
    uint32_t max_air_us;      /**< Configurable airtime cap                      */

    /* ── Per-hour total airtime cap (all types combined) ─────────────── */
    uint32_t hour_window_start_ms;               /**< Hour-window start           */
    uint32_t hour_total_air_us;                  /**< Total airtime this hour     */
    uint32_t max_hour_air_us; /**< Hour airtime cap (FWDBUDGET_MAX_HOUR_AIR_US)  */

    /* ── Drop diagnostics ──────────────────────────────────────────── */
    uint16_t fwd_drop_count[FWDBUDGET_PKT_TYPES]; /**< Drops per type this window */
    uint32_t total_fwd_drops;                    /**< All-time budget drop count  */

    /* ── Originated TX tracking (separate from relayed traffic) ─────── *
     * Counting originated and forwarded frames separately prevents a    *
     * heavy relay storm from silencing this node's own transmissions.   *
     * These counters are reset each minute with the per-minute window.  */
    uint16_t tx_originated_count[FWDBUDGET_PKT_TYPES]; /**< Originated TX/type/min */
} forward_budget_t;

/* ── Global state accessors ──────────────────────────────────────────────── */

/**
 * @brief Initialise all routing module state (dedupe cache + forward budget).
 *        Call once, before using any other routing_* functions.
 */
void routing_init(void);

/** Return pointer to module-internal dedupe cache (for use by glue layer). */
dedupe_cache_t   *routing_get_dedupe(void);

/** Return pointer to module-internal forward budget (for use by glue layer). */
forward_budget_t *routing_get_fwdbudget(void);

/* ── Forward budget API ──────────────────────────────────────────────────── */

/**
 * @brief Initialise a forward_budget_t (zero counters, set default caps).
 */
void routing_fwdbudget_init(forward_budget_t *fb);

/**
 * @brief Check whether forwarding a pkt_type frame is within budget.
 *
 * Rolls the window if now_ms is in a new period.
 *
 * @param fb         Forward budget state.
 * @param pkt_type   Packet type (0–FWDBUDGET_PKT_TYPES-1).
 * @param toa_us     Estimated Time-on-Air for the frame in microseconds.
 * @param now_ms     Current monotonic millisecond timestamp.
 * @return true if within budget (caller should then call routing_fwdbudget_record).
 */
bool routing_fwdbudget_check(forward_budget_t *fb,
                              uint8_t           pkt_type,
                              uint32_t          toa_us,
                              uint32_t          now_ms);

/**
 * @brief Record one forwarded frame against the budget.
 *
 * Must be called after routing_fwdbudget_check() returns true.
 */
void routing_fwdbudget_record(forward_budget_t *fb,
                               uint8_t           pkt_type,
                               uint32_t          toa_us,
                               uint32_t          now_ms);

/* ── Strict flood forward ────────────────────────────────────────────────── */

/**
 * @brief Strict flood-forward decision (Phase A).
 *
 * Executes in order:
 *  1. Dedupe check against @p cache — drop if already seen.
 *  2. TTL check — drop if pkt->ttl == 0 on arrival.
 *  3. Forward budget check — drop if @p fb would be exceeded.
 *  4. Mutate: pkt->ttl--, pkt->hop++, pkt->flags |= PKT_FLAG_RELAY.
 *  5. Return RIVR_FWD_FORWARD.
 *
 * On FORWARD the caller must re-encode @p pkt and enqueue it for TX
 * (optionally after a jitter delay from routing_jitter_ticks()).
 *
 * @param cache    Dedupe cache.
 * @param fb       Forward budget (may be NULL to skip budget check).
 * @param pkt      Mutable decoded packet header.
 * @param toa_us   Estimated ToA for the frame (used by budget check).
 * @param now_ms   Current monotonic millisecond timestamp.
 */
rivr_fwd_result_t routing_flood_forward(dedupe_cache_t   *cache,
                                         forward_budget_t *fb,
                                         rivr_pkt_hdr_t   *pkt,
                                         uint32_t          toa_us,
                                         uint32_t          now_ms);

/* ── Jitter helper ───────────────────────────────────────────────────────── */

/** Maximum forward jitter in ms (0..FORWARD_JITTER_MAX_MS inclusive). */
#define FORWARD_JITTER_MAX_MS  200u

/**
 * @brief Compute a deterministic forward-jitter delay in [0 .. max_j] ticks.
 *
 * Uses a single xorshift32 pass seeded from (src_id XOR seq * Knuth-hash).
 * Deterministic across implementations: replaying the same (src_id, seq)
 * always produces the same jitter value, which simplifies log analysis.
 *
 * @param src_id  Source node ID from the packet header.
 * @param seq     Sequence number from the packet header.
 * @param max_j   Maximum jitter window (inclusive upper bound).
 * @return        Jitter value in [0 .. max_j] ticks.
 */
uint16_t routing_jitter_ticks(uint32_t src_id, uint32_t seq, uint16_t max_j);

/**
 * @brief Compute a deterministic forward-jitter delay in milliseconds.
 *
 * Seed: src_id XOR seq XOR (pkt_type << 24) — includes pkt_type so
 * different frame types spread independently even at the same (src, seq).
 *
 * Returns a value in [0 .. FORWARD_JITTER_MAX_MS] ms.
 * Always 0 if FORWARD_JITTER_MAX_MS == 0 (allows compile-time disable).
 *
 * @param src_id    Source node ID.
 * @param seq       Sequence number.
 * @param pkt_type  Packet type byte (adds type-aware spread).
 * @return          Delay in milliseconds.
 */
uint32_t routing_forward_delay_ms(uint32_t src_id, uint32_t seq, uint8_t pkt_type);

/**
 * @brief Rough Time-on-Air estimate in microseconds (no radio_sx1262.h dependency).
 *
 * Uses the same approximation as RF_TOA_APPROX_US: SF8, BW125kHz, CR4/8.
 * Used by the forward budget without pulling in the radio driver header.
 *
 * @param payload_len  Wire-encoded payload length in bytes.
 * @return             Approximate ToA in microseconds.
 */
static inline uint32_t routing_toa_estimate_us(uint8_t payload_len)
{
    return (uint32_t)(2048u + 2048u *
        (((uint32_t)(payload_len) * 8u + 28u + 32u) / (4u * (8u - 2u))));
}

/* ════════════════════════════════════════════════════════════════════════════
 * PHASE D — Control-plane packets (ROUTE_REQ / ROUTE_RPL)
 * ══════════════════════════════════════════════════════════════════════════ */

/** Default TTL for route-request floods. */
#define ROUTE_REQ_TTL  4u

/**
 * Payload layout for ROUTE_RPL (9 bytes):
 *   [0–3]  target_id   u32 LE — the destination node we have a route to
 *   [4–7]  next_hop    u32 LE — our next hop toward target_id
 *   [8]    hop_count   u8     — hops from replier to target_id
 */
#define ROUTE_RPL_PAYLOAD_LEN  9u

/**
 * @brief Build a PKT_ROUTE_REQ wire frame (broadcast).
 *
 * Encodes a ROUTE_REQ to find a path to @p target_id.
 * Sets dst_id = @p target_id so intermediate nodes can recognise it.
 * TTL = ROUTE_REQ_TTL; flags = 0; seq must be supplied by caller.
 *
 * @return Number of bytes written, or -1 on buffer overflow.
 */
int routing_build_route_req(uint32_t  my_id,
                             uint32_t  target_id,
                             uint32_t  seq,
                             uint8_t  *out_buf,
                             uint8_t   out_cap);

/**
 * @brief Build a PKT_ROUTE_RPL wire frame (unicast back to requester).
 *
 * The payload carries { target_id, next_hop, hop_count } so the requester
 * can populate its route cache in one step.
 *
 * @param my_id        ID of the node sending the reply.
 * @param requester_id Node that sent the ROUTE_REQ (becomes dst_id).
 * @param target_id    The destination about which we are replying.
 * @param next_hop     Our known next hop toward target_id.
 * @param hop_count    Hops from us to target_id.
 * @param seq          Sequence number (caller-managed).
 * @param out_buf / out_cap  Output buffer.
 * @return Number of bytes written, or -1 on buffer overflow.
 */
int routing_build_route_rpl(uint32_t  my_id,
                             uint32_t  requester_id,
                             uint32_t  target_id,
                             uint32_t  next_hop,
                             uint8_t   hop_count,
                             uint32_t  seq,
                             uint8_t  *out_buf,
                             uint8_t   out_cap);

/**
 * @brief Check if an inbound ROUTE_REQ is asking about a target we know.
 *
 * @p pkt must have pkt_type == PKT_ROUTE_REQ.
 * pkt->dst_id is the requested target.
 *
 * @param pkt  Decoded ROUTE_REQ header.
 * @return true if this node should send a ROUTE_RPL (i.e. we ARE the target
 *         or we have a cached route to pkt->dst_id).
 */
bool routing_should_reply_route_req(const rivr_pkt_hdr_t *pkt,
                                     uint32_t              my_id);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_ROUTING_H */
