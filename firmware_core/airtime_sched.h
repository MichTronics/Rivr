/**
 * @file  airtime_sched.h
 * @brief Airtime token-bucket scheduler — packet-class fairness gate.
 *
 * Layered on top of the global duty-cycle limiter (dutycycle.h/.c).
 * The duty-cycle check remains the ABSOLUTE regulatory floor; this module
 * provides a SOFTER, class-aware fairness gate that fires first.
 *
 * Packet classes (priority order):
 *   PKTCLASS_CONTROL  – BEACON, ROUTE_REQ, ROUTE_RPL, ACK, PROG_PUSH
 *                       Always passes: never rate-limited.
 *   PKTCLASS_CHAT     – PKT_CHAT (user messages)
 *   PKTCLASS_METRICS  – PKT_DATA (sensor / telemetry payloads)
 *   PKTCLASS_BULK     – all unrecognised types (future)
 *
 * Global token bucket:
 *   Capacity   : AIRTIME_CAPACITY_US        (10 s = 10 000 000 µs)
 *   Refill rate: AIRTIME_REFILL_US_PER_MS   (100 µs/ms = 10 % duty)
 *   Low mark   : AIRTIME_LOW_WATERMARK_US   (10 % of capacity = 1 000 000 µs)
 *
 * Per-neighbour token buckets (relay fairness):
 *   Applied only to frames with PKT_FLAG_RELAY set (forwarded traffic).
 *   Each remote source gets an independent sub-bucket so one chatty
 *   neighbour cannot monopolise all forwarding airtime.
 *   Capacity   : AIRTIME_NB_CAPACITY_US       (2 s = 2 000 000 µs)
 *   Refill rate: AIRTIME_NB_REFILL_US_PER_MS  (20 µs/ms ≅ 2 % duty)
 *
 * Metrics incremented here (defined in rivr_metrics.h):
 *   g_rivr_metrics.airtime_tokens_low  – global bucket crossed low-watermark
 *   g_rivr_metrics.class_drops_ctrl    – CONTROL drops (always 0 if firmware OK)
 *   g_rivr_metrics.class_drops_chat    – CHAT frames dropped by token gate
 *   g_rivr_metrics.class_drops_metrics – METRICS frames dropped
 *   g_rivr_metrics.class_drops_bulk    – BULK frames dropped
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tuning constants ────────────────────────────────────────────────────── */

/** Global bucket capacity in µs (10-second burst window). */
#define AIRTIME_CAPACITY_US          10000000u

/** Token refill rate: µs added per millisecond elapsed.
 *  100 µs/ms → 100 ms/s → 360 s/hour → exactly 10 % duty over an hour. */
#define AIRTIME_REFILL_US_PER_MS     100u

/** Threshold below which airtime_tokens_low metric is incremented (10 %). */
#define AIRTIME_LOW_WATERMARK_US     1000000u

/** Per-neighbour bucket capacity in µs (2-second burst window). */
#define AIRTIME_NB_CAPACITY_US       2000000u

/** Per-neighbour refill rate in µs/ms (≅ 2 % duty per source). */
#define AIRTIME_NB_REFILL_US_PER_MS  20u

/** Maximum tracked source nodes (matches NEIGHBOR_TABLE_SIZE). */
#define AIRTIME_NB_MAX               16u

/* ── Packet class enum ───────────────────────────────────────────────────── */

/** Priority class of a packet, derived from pkt_type. */
typedef enum {
    PKTCLASS_CONTROL = 0,  /**< Always passes, never counted against budget  */
    PKTCLASS_CHAT    = 1,  /**< User messages — rate-limited under congestion */
    PKTCLASS_METRICS = 2,  /**< Sensor / telemetry data                       */
    PKTCLASS_BULK    = 3,  /**< Unrecognised / future bulk application data   */
} rivr_pkt_class_t;

/* ── Per-neighbour sub-bucket ────────────────────────────────────────────── */

typedef struct {
    uint32_t node_id;         /**< Source node ID (0 = slot free)            */
    uint32_t tokens_us;       /**< Available airtime for this source (µs)    */
    uint32_t last_refill_ms;  /**< tb_millis() of last refill                 */
} airtime_nb_bucket_t;

/* ── Global airtime context ──────────────────────────────────────────────── */

typedef struct {
    uint32_t tokens_us;                    /**< Global available tokens (µs)   */
    uint32_t last_refill_ms;               /**< Global last refill timestamp    */
    airtime_nb_bucket_t nb[AIRTIME_NB_MAX];/**< Per-source relay buckets        */
} airtime_ctx_t;

extern airtime_ctx_t g_airtime;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise the airtime scheduler to full capacity.
 *        Call once from app_main() after timebase_init().
 */
void airtime_sched_init(void);

/**
 * @brief Map a raw pkt_type byte to a rivr_pkt_class_t.
 *
 * CONTROL class: PKT_BEACON(2), PKT_ROUTE_REQ(3), PKT_ROUTE_RPL(4),
 *                PKT_ACK(5), PKT_PROG_PUSH(7)
 * CHAT class:    PKT_CHAT(1)
 * METRICS class: PKT_DATA(6)
 * BULK class:    everything else (future extension point)
 */
rivr_pkt_class_t rivr_pkt_classify(uint8_t pkt_type);

/**
 * @brief Check whether the frame may be transmitted; consume tokens if yes.
 *
 * Decision logic:
 *   1. Classify pkt_type at frame_data[PKT_TYPE_BYTE_OFFSET].
 *   2. CONTROL → always allow (no token consumption).
 *   3. Refill global bucket based on elapsed time.
 *   4. If global tokens < toa_us → drop (increment class_drops_*) → false.
 *   5. Consume global tokens.
 *   6. If global tokens drop below AIRTIME_LOW_WATERMARK_US → increment
 *      airtime_tokens_low metric.
 *   7. If frame has PKT_FLAG_RELAY set → also check per-source sub-bucket:
 *      if source is over budget → undo global consume → drop → false.
 *   8. Return true.
 *
 * @param frame_data  Raw wire-format frame bytes (may be NULL if frame_len==0).
 * @param frame_len   Number of valid bytes in frame_data.
 * @param toa_us      Estimated time-on-air for this frame (µs).
 * @param now_ms      Current tb_millis() value.
 * @return true  Frame is allowed; tokens have been consumed.
 * @return false Frame is dropped by fairness gate.
 */
bool airtime_sched_check_consume(const uint8_t *frame_data,
                                 uint8_t        frame_len,
                                 uint32_t       toa_us,
                                 uint32_t       now_ms);

#ifdef __cplusplus
}
#endif
