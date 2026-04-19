/**
 * @file  routing.h
 * @brief RIVR mesh routing layer — duplicate suppression, TTL management,
 *        flood-forward hardening (Phase A), and hybrid unicast support (Phase D).
 *
 * PACKET IDENTITY MODEL
 *  Two orthogonal u16 fields replace the old single u32 seq:
 *
 *  │ seq    (u16) │ Application/source sequence. Per-source message counter,
 *  │              │ preserved through relay. Used for message ordering and
 *  │              │ control-plane correlation (ROUTE_REQ/RPL, future ACK).
 *  │              │
 *  │ pkt_id (u16) │ Per-injection forwarding identity. Unique per wire
 *  │              │ injection (new for retransmits, fallback floods, and
 *  │              │ every new control-plane frame). Dedupe cache keys on
 *  │              │ (src_id, pkt_id). Jitter seeds from pkt_id.
 *
 *  Result: fallback floods (same seq, new pkt_id) bypass dedupe at nodes that
 *  already saw the original unicast.  Retransmits with the same pkt_id are
 *  correctly deduped.  Control-plane frames use a monotonic pkt_id counter
 *  (g_ctrl_seq) that also serves as the seq correlation token.
 *
 * PHASE A — Flood hardening:
 *  1. `routing_flood_forward()` — strict flood decision with explicit
 *     DEDUPE / TTL / LOOP / BUDGET drop reasons.
 *  2. `routing_loop_guard_hash()` — 8-bit relay fingerprint for loop_guard field.
 *  3. `routing_jitter_ticks()` — deterministic xorshift jitter in [0..J].
 *  4. `forward_budget_t` — per-pkt_type forwards/minute + airtime cap so
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
#include "route_cache.h"   /* route_cache_t, route_cache_lookup() */
#include "hal/feature_flags.h"  /* RIVR_ROLE_REPEATER for budget scaling */
#include "rivr_config.h"        /* RF_SPREADING_FACTOR, RF_BANDWIDTH_HZ   */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Dedupe cache ──────────────────────────────────────────────────────────── *
 *
 * A power-of-2 ring buffer (DEDUPE_CACHE_SIZE entries) that stores the most
 * recently seen (src_id, pkt_id) pairs.  On each new packet:
 *   - Scan the ring for a matching entry → duplicate, suppress.
 *   - If not found → insert at `head`, advance head (oldest entry evicted).
 *
 * Expiry: entries older than DEDUPE_EXPIRY_MS are treated as not-seen so that
 * a legitimate retransmit after a long silence is accepted.
 * ────────────────────────────────────────────────────────────────────────── */

#define DEDUPE_CACHE_SIZE    64u    /**< Ring-buffer capacity (power of 2)   */
#define DEDUPE_EXPIRY_MS  60000u    /**< Entry expiry: 60 seconds            */

/** Single deduplicate-cache entry. */
typedef struct {
    uint32_t src_id;       /**< Source node ID (0 = empty slot)             */
    uint16_t pkt_id;       /**< Per-injection forwarding identity (dedupe key) */
    uint16_t _pad;         /**< Alignment padding                           */
    uint32_t seen_at_ms;   /**< Monotonic ms timestamp of first arrival     */
} dedupe_entry_t;

/** Complete duplicate-suppression state. */
typedef struct {
    dedupe_entry_t entries[DEDUPE_CACHE_SIZE];
    uint8_t        head;   /**< Next write position (ring head / oldest)    */
} dedupe_cache_t;

/* ── Neighbor table ─────────────────────────────────────────────────────── */

#define NEIGHBOR_TABLE_SIZE  16u     /**< Maximum tracked neighbours          */
#define NEIGHBOR_EXPIRY_MS   120000u /**< Evict neighbours unseen for 2 min   */

/** One entry in the neighbour table. */
typedef struct {
    uint32_t  node_id;        /**< Neighbour node ID (0 = empty slot)       */
    uint32_t  last_seen_ms;   /**< Monotonic ms of last received packet     */
    int8_t    rssi_dbm;       /**< EWMA of observed RSSI (α=1/8)            */
    uint8_t   hop_count;      /**< Minimum hops to this neighbour (=hop+1)  */
    char      callsign[12];   /**< Callsign from last PKT_BEACON (NUL-term) */
    /* ── Step 5: per-neighbor link stats ─────────────────────────────── */
    uint32_t  rx_ok;          /**< Count of valid frames received           */
    int8_t    last_snr_db;    /**< EWMA of observed SNR in dB (α=1/8)      */
    uint8_t   role;           /**< Node role from last PKT_BEACON (rivr_node_role_t, 0=unknown) */
    uint8_t   _pad[2];        /**< Alignment padding                        */
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
 * @brief Check whether (src_id, pkt_id) is genuinely new.
 *
 * If new: inserts the entry into the ring (evicting the oldest if full) and
 * returns true.
 * If already seen (and not yet expired): returns false.
 *
 * @param cache      Dedupe cache state.
 * @param src_id     Sender node ID.
 * @param pkt_id     Per-injection forwarding identity (NOT application seq).
 * @param now_ms     Current monotonic millisecond timestamp.
 *
 * @return true  → packet is new, inject / forward it.
 * @return false → duplicate, suppress it.
 */
bool routing_dedupe_check(dedupe_cache_t *cache,
                          uint32_t        src_id,
                          uint16_t        pkt_id,
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
 * RSSI and SNR are maintained as EWMA (α=1/8) for oscillation resistance.
 *
 * @param tbl       Neighbour table.
 * @param pkt       Decoded packet header (src_id, hop used).
 * @param rssi_dbm  Observed RSSI of the received frame.
 * @param snr_db    Observed SNR of the received frame (dB).
 * @param now_ms    Current monotonic millisecond timestamp.
 */
void routing_neighbor_update(neighbor_table_t       *tbl,
                             const rivr_pkt_hdr_t   *pkt,
                             int8_t                  rssi_dbm,
                             int8_t                  snr_db,
                             uint32_t                now_ms);

/**
 * @brief Return the number of alive (non-expired) neighbour entries.
 */
uint8_t routing_neighbor_count(const neighbor_table_t *tbl, uint32_t now_ms);

/**
 * @brief Return a pointer to neighbour entry at raw index @p idx, or NULL if
 *        the slot is empty (node_id == 0) or @p idx is out of range.
 *
 * The returned pointer is valid until the next call to routing_neighbor_update().
 * Index is 0-based into the internal table; iterate 0..NEIGHBOR_TABLE_SIZE-1.
 */
const neighbor_entry_t *routing_neighbor_get(const neighbor_table_t *tbl,
                                              uint8_t idx);

/**
 * @brief Store a callsign string into the entry matching @p node_id.
 *
 * If no entry with @p node_id exists (beacon received before any other packet
 * from this node) the call is a no-op.  The string is copied and NUL-terminated.
 *
 * @param tbl       Neighbour table.
 * @param node_id   Node to update.
 * @param callsign  NUL-terminated callsign; at most 11 chars are copied.
 */
void routing_neighbor_set_callsign(neighbor_table_t *tbl,
                                   uint32_t          node_id,
                                   const char       *callsign);

/**
 * @brief Store a role value into the entry matching @p node_id.
 */
void routing_neighbor_set_role(neighbor_table_t *tbl,
                               uint32_t          node_id,
                               uint8_t           role);

/**
 * @brief Compute the effective link score (0..100) for a neighbour.
 *
 * Formula (all integer arithmetic, no floating point):
 *   rssi_part = clamp(rssi_dbm + 140, 0, 80)   — best at -60 dBm
 *   snr_part  = clamp(snr_db   +  10, 0, 20)   — best at ≥+10 dB
 *   base      = rssi_part + snr_part             — 0..100
 *   score     = base × (NEIGHBOR_EXPIRY_MS − age_ms) / NEIGHBOR_EXPIRY_MS
 *
 * Score decays linearly to 0 as the link approaches NEIGHBOR_EXPIRY_MS,
 * providing automatic aging without separate expiry logic.
 * Returns 0 if the entry is already expired.
 */
uint8_t routing_neighbor_link_score(const neighbor_entry_t *n,
                                     uint32_t                now_ms);

/**
 * @brief Print the neighbour table as a human-readable table via printf().
 *
 * Output format (one row per live entry):
 *   NodeID      Callsign    Hops  RSSI(dB)  SNR(dB)  Score  Age(s)  rx_ok
 *
 * @param tbl     Neighbour table.
 * @param now_ms  Current monotonic millisecond timestamp.
 */
void routing_neighbor_print(const neighbor_table_t *tbl, uint32_t now_ms);

/* Pull in the standalone neighbor-table types (rivr_neighbor_table_t) so
 * that routing_next_hop_score() can accept the richer quality table. */
#include "neighbor_table.h"

/**
 * @brief Quality score for a candidate next-hop from the standalone neighbor
 *        table.
 *
 * Wraps neighbor_find() + neighbor_link_score() and returns 0 when the
 * candidate is unknown or its entry has expired.  Callers in the TX path
 * compare this score against NTABLE_SCORE_UNICAST_MIN to decide whether to
 * proceed with a unicast or fall back to a flood.
 *
 * @param ntbl          Standalone neighbor table (rivr_neighbor_table_t).
 * @param candidate_id  Node ID of the proposed next-hop.
 * @param now_ms        Current monotonic millisecond timestamp.
 * @return Quality score in [0, 100]; 0 means unknown / expired.
 */
uint8_t routing_next_hop_score(const rivr_neighbor_table_t *ntbl,
                               uint32_t                     candidate_id,
                               uint32_t                     now_ms);

/* ════════════════════════════════════════════════════════════════════════════
 * PHASE A — Flood hardening
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Forward result ──────────────────────────────────────────────────────── */

typedef enum {
    RIVR_FWD_FORWARD,        /**< Packet accepted; pkt modified in place      */
    RIVR_FWD_DROP_DEDUPE,    /**< Duplicate — suppress                         */
    RIVR_FWD_DROP_TTL,       /**< TTL exhausted (already 0 on arrival)         */
    RIVR_FWD_DROP_BUDGET,    /**< Safety forward budget exceeded               */
    RIVR_FWD_DROP_LOOP,      /**< Loop detected via loop_guard fingerprint     */
} rivr_fwd_result_t;

/* ── Safety forward budget ───────────────────────────────────────────────── *
 *
 * Maintains independent counters per pkt_type (0–FWDBUDGET_PKT_TYPES-1).
 * Both a count-per-minute cap and an airtime-per-minute cap are enforced.
 * Counters reset each time now_ms crosses a new FWDBUDGET_WINDOW_MS boundary.
 * ─────────────────────────────────────────────────────────────────────────── */

#define FWDBUDGET_PKT_TYPES    8u           /**< Types 0–7                     */
#define FWDBUDGET_WINDOW_MS    60000u       /**< Rolling 1-minute window        */
#define FWDBUDGET_MAX_FWD      30u          /**< Max forwards / type / minute (base) */
#define FWDBUDGET_MAX_AIR_US   3000000u     /**< Max airtime / type / minute    */

/**
 * Role-specific forward rate cap (applied by routing_fwdbudget_init).
 *   REPEATER : 60 fwd/type/min — optimised for relay throughput.
 *   CLIENT   : 20 fwd/type/min — conservative; client is not a relay hub.
 *   generic  : 30 fwd/type/min — base constant (sim/test default).
 */
#if RIVR_ROLE_REPEATER
#  define FWDBUDGET_MAX_FWD_ROLE  60u  /**< relay hub — high throughput        */
#elif RIVR_ROLE_GATEWAY
#  define FWDBUDGET_MAX_FWD_ROLE  60u  /**< bridge — relay everything to IP    */
#elif RIVR_ROLE_CLIENT
#  define FWDBUDGET_MAX_FWD_ROLE  20u  /**< end-device — conservative relay    */
#else
#  define FWDBUDGET_MAX_FWD_ROLE  FWDBUDGET_MAX_FWD  /**< sim/test: use base 30 */
#endif
                                            /**  (3 s = ~5% duty on SF9 BW125) */

/**
 * Role-specific airtime cap — mirrors FWDBUDGET_MAX_FWD_ROLE above.
 * In sim/test builds (no role defined) disable the airtime cap so
 * flood-correctness tests are not sensitive to RF parameter changes.
 */
#if RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY || RIVR_ROLE_CLIENT
#  define FWDBUDGET_MAX_AIR_US_ROLE  FWDBUDGET_MAX_AIR_US
#else
#  define FWDBUDGET_MAX_AIR_US_ROLE  UINT32_MAX  /**< sim/test: airtime uncapped */
#endif

/* Hour-level airtime cap (independent of the per-minute per-type caps). */
#define FWDBUDGET_HOUR_WINDOW_MS     3600000u   /**< 1-hour rolling window        */
#define FWDBUDGET_MAX_HOUR_AIR_US    360000000u /**< 360 s / hour = 10% duty cycle */

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

/**
 * @brief Adapt forward-budget caps to measured channel load (Phase 3).
 *
 * Call once per minute (FWDBUDGET_WINDOW_MS boundary) from the main loop.
 * When RIVR_FEATURE_ADAPTIVE_FLOOD=0 this is a no-op (zero overhead).
 *
 * Scaling rule (three tiers):
 *   load  < 40  → full cap (FWDBUDGET_MAX_FWD_ROLE)
 *   load 40–69  → 50 % of full cap
 *   load ≥ 70   → 25 % of full cap  (floor 4  — node never fully silenced)
 *
 * where load = max(fabric_score, dc_pct).
 *
 * @param fb            Forward budget to adapt.
 * @param fabric_score  Congestion score 0–100 from rivr_fabric_get_score();
 *                      pass 0 when RIVR_FABRIC_REPEATER=0.
 * @param dc_pct        Duty-cycle used, percent 0–100 over the hour window;
 *                      derive with dutycycle_remaining_us() / DC_BUDGET_US.
 */
void routing_fwdbudget_adapt(forward_budget_t *fb,
                              uint8_t           fabric_score,
                              uint8_t           dc_pct);

/* ── Strict flood forward ────────────────────────────────────────────────── */

/**
 * @brief Fold a 32-bit node ID to an 8-bit relay fingerprint for loop_guard.
 *
 * Each relay XORs all four bytes of its node_id then ensures the result is
 * non-zero (0 means "not set" in the OR-accumulator).  The hash is the same
 * across all nodes in the mesh, so the same node always produces the same
 * fingerprint — deterministic and replay-testable.
 *
 * @param node_id  The relay node's own unique ID.
 * @return         A non-zero 8-bit fingerprint.
 */
static inline uint8_t routing_loop_guard_hash(uint32_t node_id)
{
    uint8_t h = (uint8_t)(    node_id          &  0xFFu)
              ^ (uint8_t)((node_id >>  8u)     &  0xFFu)
              ^ (uint8_t)((node_id >> 16u)     &  0xFFu)
              ^ (uint8_t)((node_id >> 24u)     &  0xFFu);
    return (h != 0u) ? h : 1u;   /* 0 is reserved for "originator" */
}

/**
 * @brief Strict flood-forward decision (Phase A).
 *
 * Executes in order:
 *  1. Dedupe check against @p cache — drop if already seen.
 *  2. TTL check — drop if pkt->ttl == 0 on arrival.
 *  3. Loop-guard check — if @p my_id != 0, compute h=routing_loop_guard_hash(my_id)
 *     and drop (RIVR_FWD_DROP_LOOP) when (pkt->loop_guard & h) == h, meaning
 *     this node's fingerprint is already set — the packet has looped back here.
 *  4. Forward budget check — drop if @p fb would be exceeded.
 *  5. Mutate: pkt->ttl--, pkt->hop++, pkt->flags |= PKT_FLAG_RELAY,
 *             pkt->loop_guard |= h  (records this relay's fingerprint).
 *  6. Return RIVR_FWD_FORWARD.
 *
 * On FORWARD the caller must re-encode @p pkt and enqueue it for TX
 * (optionally after a jitter delay from routing_jitter_ticks()).
 *
 * @param cache    Dedupe cache.
 * @param fb       Forward budget (may be NULL to skip budget check).
 * @param pkt      Mutable decoded packet header.
 * @param my_id    This node's own ID (0 = skip loop-guard check).
 * @param toa_us   Estimated ToA for the frame (used by budget check).
 * @param now_ms   Current monotonic millisecond timestamp.
 */
rivr_fwd_result_t routing_flood_forward(dedupe_cache_t   *cache,
                                         forward_budget_t *fb,
                                         rivr_pkt_hdr_t   *pkt,
                                         uint32_t          my_id,
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
uint16_t routing_jitter_ticks(uint32_t src_id, uint16_t pkt_id, uint16_t max_j);

/**
 * @brief Compute a deterministic forward-jitter delay in milliseconds.
 *
 * Seed: src_id XOR pkt_id XOR (pkt_type << 24) — includes pkt_type so
 * different frame types spread independently even at the same (src, pkt_id).
 * Using pkt_id rather than seq means fallback floods (same seq, new pkt_id)
 * get their own independent jitter slot.
 *
 * Returns a value in [0 .. FORWARD_JITTER_MAX_MS] ms.
 * Always 0 if FORWARD_JITTER_MAX_MS == 0 (allows compile-time disable).
 *
 * @param src_id    Source node ID.
 * @param pkt_id    Per-injection forwarding identity.
 * @param pkt_type  Packet type byte (adds type-aware spread).
 * @return          Delay in milliseconds.
 */
uint32_t routing_forward_delay_ms(uint32_t src_id, uint16_t pkt_id, uint8_t pkt_type);

/**
 * @brief Rough Time-on-Air estimate in microseconds (no radio_sx1262.h dependency).
 *
 * Parameterised by RF_SPREADING_FACTOR and RF_BANDWIDTH_HZ to match
 * RF_TOA_APPROX_US across all variants (e.g. BW=62500 Hz for the E22-900).
 * Used by the forward budget without pulling in the radio driver header.
 *
 *   T_sym_us   = 2^SF * 1e6 / BW_Hz
 *   t_preamble = 49 * T_sym_us / 4   (= 12.25 × T_sym)
 *   n_payload  = floor((8×PL + 43) / 32) × 8   [CR4/8]
 *   ToA        = t_preamble + n_payload × T_sym_us
 *
 * @param payload_len  Wire-encoded payload length in bytes.
 * @return             Approximate ToA in microseconds.
 */
static inline uint32_t routing_toa_estimate_us(uint8_t payload_len)
{
    const uint32_t t_sym =
        (uint32_t)(((uint64_t)1u << RF_SPREADING_FACTOR) * 1000000ull / RF_BANDWIDTH_HZ);
    return (49u * t_sym / 4u) +
           (((8u * (uint32_t)payload_len + 43u) / 32u) * 8u * t_sym);
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
 * TTL = ROUTE_REQ_TTL; flags = 0.
 *
 * @param seq     Application sequence / correlation token (u16).
 * @param pkt_id  Per-injection forwarding identity (u16); a fresh value from
 *                g_ctrl_seq ensures this frame is not deduped against any
 *                previous ROUTE_REQ for the same target.
 *
 * @return Number of bytes written, or -1 on buffer overflow.
 */
int routing_build_route_req(uint32_t  my_id,
                             uint32_t  target_id,
                             uint16_t  seq,
                             uint16_t  pkt_id,
                             uint8_t  *out_buf,
                             uint8_t   out_cap);

/**
 * @brief Build a PKT_ROUTE_RPL wire frame.
 *
 * ROUTE_RPL uses **directed-flood** semantics — the same TTL as ROUTE_REQ
 * so it floods back through the mesh toward the requester.  Only the node
 * whose node_id matches dst_id (= requester_id) needs to act on it, but
 * any node that overhears it may learn the route as a beneficial side-effect.
 *
 * The 9-byte payload carries { target_id, next_hop, hop_count }:
 *   target_id  — the destination the requester was seeking
 *   next_hop   — the replier's own next hop toward target_id
 *                (= my_id when we ARE the target; = cache.next_hop otherwise)
 *   hop_count  — hops from the replier to target_id
 *                (= 0 when we ARE the target; = cache.hop_count otherwise)
 *
 * The requester reconstructs the total hop count as:
 *   total_hops = hop_count + pkt_hdr.hop + 1
 * where pkt_hdr.hop is the number of relay hops the ROUTE_RPL took to
 * reach the requester.
 *
 * @param my_id        ID of the node sending the reply.
 * @param requester_id Node that sent the ROUTE_REQ (becomes dst_id).
 * @param target_id    The destination about which we are replying.
 * @param next_hop     Our next hop toward target_id (my_id if we are target).
 * @param hop_count    Hops from us to target_id (0 if we are target).
 * @param seq          Sequence number / correlation token (u16).
 * @param pkt_id        Per-injection forwarding identity (u16).
 * @param out_buf / out_cap  Output buffer.
 * @return Number of bytes written, or -1 on buffer overflow.
 */
int routing_build_route_rpl(uint32_t  my_id,
                             uint32_t  requester_id,
                             uint32_t  target_id,
                             uint32_t  next_hop,
                             uint8_t   hop_count,
                             uint16_t  seq,
                             uint16_t  pkt_id,
                             uint8_t  *out_buf,
                             uint8_t   out_cap);

/**
 * @brief Decide whether this node should send a ROUTE_RPL for an inbound
 *        ROUTE_REQ.
 *
 * Returns true in exactly two cases:
 *
 *  1. **We are the requested destination** — pkt->dst_id == my_id.
 *     Reply payload: { target=my_id, next_hop=my_id, hop_count=0 }.
 *
 *  2. **We have an eligible cached route** for pkt->dst_id, as determined
 *     by route_cache_can_reply_for_dst() (see route_cache.h for the full
 *     reply-eligibility policy):
 *       a. Entry is valid and non-expired.
 *       b. RCACHE_FLAG_PENDING is NOT set (route is confirmed).
 *       c. entry->metric >= RCACHE_REPLY_MIN_METRIC (not a marginal path).
 *       d. entry->hop_count <= RCACHE_REPLY_MAX_HOPS (not a deep chain).
 *     Reply payload: { target=pkt->dst_id,
 *                      next_hop=cache_entry.next_hop,
 *                      hop_count=cache_entry.hop_count }.
 *
 * All other cases return false (invalid pkt, unknown destination, expired,
 * pending, weak, or too-deep cached route).
 *
 * @param pkt      Decoded ROUTE_REQ header (pkt_type must be PKT_ROUTE_REQ).
 * @param my_id    This node's own identifier.
 * @param cache    Route cache to consult for case 2 (may be NULL → only
 *                 case 1 is checked, as before).
 * @param now_ms   Monotonic millisecond timestamp (for expiry test).
 * @return true  → caller should build and send a ROUTE_RPL.
 * @return false → do not reply.
 */
bool routing_should_reply_route_req(const rivr_pkt_hdr_t *pkt,
                                     uint32_t              my_id,
                                     route_cache_t        *cache,
                                     uint32_t              now_ms);

/**
 * @brief Build a PKT_ACK wire frame.
 *
 * The 6-byte payload carries { ack_src_id (u32 LE), ack_pkt_id (u16 LE) }.
 * These identify which of the sender's retry-table entries should be cleared
 * upon receipt.
 *
 * @param my_id        ID of the node sending the ACK (= the frame's final dst).
 * @param dst_id       Node that should receive the ACK (= pkt_hdr.src_id).
 * @param ack_src_id   src_id of the frame being acknowledged.
 * @param ack_pkt_id   pkt_id currently active in the sender's retry entry.
 * @param seq          Sequence number for this ACK frame (u16).
 * @param pkt_id       Per-injection forwarding identity for this ACK (u16).
 * @param out_buf / out_cap  Output buffer.
 * @return Number of bytes written, or -1 on buffer overflow.
 */
int routing_build_ack(uint32_t my_id,
                       uint32_t dst_id,
                       uint32_t ack_src_id,
                       uint16_t ack_pkt_id,
                       uint16_t seq,
                       uint16_t pkt_id,
                       uint8_t *out_buf,
                       uint8_t  out_cap);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_ROUTING_H */
