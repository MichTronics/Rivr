/**
 * @file  rivr_gps.h
 * @brief GPS + location-advertisement service for the Rivr mesh.
 *
 * Architecture overview
 * ─────────────────────
 * All GPS frames ride PKT_DATA with a 2-byte service header:
 *   [0] svc      = RIVR_DATA_SVC_GPS (0x01)
 *   [1] subtype  = GPS_SUB_*
 *
 * Subtypes and payload sizes:
 *   ADVERT    8  B  – coarse position + mobility, very frequent, low-TTL
 *   META     14  B  – fine fixed position for static nodes (once at boot)
 *   POS_REQ   4  B  – unicast request for a fine fix
 *   POS_RESP 12  B  – unicast reply with fine fix + speed
 *   TRACK_CTL 3  B  – start/stop live tracking (unused in base impl)
 *
 * Node behaviour
 * ──────────────
 * STATIC (repeater/gateway):
 *   boot  → send META once
 *   every 30–60 min (jittered) → send ADVERT
 *   on POS_REQ → send POS_RESP
 *
 * MOBILE (client with GPS):
 *   NO_FIX → wait for GNSS
 *   FIX_ACQUIRE → first fix → send ADVERT + POS_RESP
 *   TRACK_IDLE  → event-driven only (distance/heading/timeout)
 *   TRACK_MOVING/TRACK_FAST → more frequent ADVERT on significant motion
 *
 * Hard constraints
 * ────────────────
 *   – No heap allocation (all state in BSS)
 *   – No floating-point in the hot path (fixed-point arithmetic only)
 *   – No blocking calls; all functions return immediately
 *   – ADVERT TTL ≤ 1 (local neighbourhood only)
 */

#ifndef RIVR_GPS_H
#define RIVR_GPS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Service + subtype constants ─────────────────────────────────────────── */

/** GPS service identifier (first byte of PKT_DATA payload). */
#define RIVR_DATA_SVC_GPS    0x01u

/* ── Static-node pre-configured position ────────────────────────────────── *
 * These may be overridden via a -D build flag, a variants/ config.h, or    *
 * sdkconfig (via platformio.ini build_flags).  Defaults to 0,0 (unknown). *
 * Units: degrees × 10⁻⁵ (int32_t) and metres (int16_t).                   */
#ifndef RIVR_STATIC_LAT_E5
#define RIVR_STATIC_LAT_E5  0
#endif
#ifndef RIVR_STATIC_LON_E5
#define RIVR_STATIC_LON_E5  0
#endif
#ifndef RIVR_STATIC_ALT_M
#define RIVR_STATIC_ALT_M   0
#endif

/** GPS sub-packet types (second byte). */
#define GPS_SUB_ADVERT       0x01u  /**< Coarse pos + mobility (8 B)         */
#define GPS_SUB_META         0x02u  /**< Fine fixed pos for static nodes (14B)*/
#define GPS_SUB_POS_REQ      0x03u  /**< Request fine position (4 B)         */
#define GPS_SUB_POS_RESP     0x04u  /**< Fine position reply (12 B)          */
#define GPS_SUB_TRACK_CTL    0x05u  /**< Start/stop tracking (3 B, reserved) */

/* ── ADVERT flags ────────────────────────────────────────────────────────── */

/** Position data is fresh (age < 5 min). */
#define GPS_FLAG_POS_VALID   0x01u
/** Node has GPS hardware (may lack fix). */
#define GPS_FLAG_HAS_GPS     0x02u
/** Node is a mesh infrastructure node (repeater/gateway). */
#define GPS_FLAG_INFRA       0x04u

/* ── Quantisation helpers ────────────────────────────────────────────────── */

/**
 * Coarse grid encoding: 1 LSB ≈ 150–300 m depending on latitude.
 *   lat_q = (int16_t)(lat_e5 / 300)    [–10922..+10922 for ±90°]
 *   lon_q = (int16_t)(lon_e5 / 300)    [–21845..+21845 for ±180°]
 *
 * Decode: lat_e5 ≈ lat_q * 300  (multiply by 300 to get degrees×10⁻⁵)
 */
#define GPS_LAT_Q_FROM_E5(x)  ((int16_t)((x) / 300))
#define GPS_LON_Q_FROM_E5(x)  ((int16_t)((x) / 300))
#define GPS_LAT_Q_TO_E5(q)    ((int32_t)(q) * 300)
#define GPS_LON_Q_TO_E5(q)    ((int32_t)(q) * 300)

/* ── Wire-format structs (all packed) ───────────────────────────────────── */

/**
 * GPS_SUB_ADVERT — 8 bytes total.
 *
 * Sent periodically and on significant position change.
 * TTL = 1 (local neighbourhood only — no mesh-wide flooding).
 *
 * The `flags` byte carries both control bits in [5:0] and the mobility class
 * in bits [7:6], so that the struct fits exactly 8 bytes on the wire.
 */
typedef struct __attribute__((packed)) {
    uint8_t  svc;         /**< RIVR_DATA_SVC_GPS                             */
    uint8_t  subtype;     /**< GPS_SUB_ADVERT                                */
    uint8_t  flags;       /**< [7:6]=mobility (rivr_mobility_t), [5:0]=GPS_FLAG_* */
    uint8_t  state_seq;   /**< Wrapping counter; increment on state change   */
    int16_t  lat_q;       /**< Coarse latitude  (~150–300 m resolution)      */
    int16_t  lon_q;       /**< Coarse longitude (~150–300 m resolution)      */
} rivr_gps_advert_t;

/** Pack mobility class (0–2) and GPS_FLAG_* bits into the advert flags byte. */
#define GPS_ADVERT_FLAGS_PACK(f, m)   ((uint8_t)(((f) & 0x3Fu) | (((m) & 0x03u) << 6)))
/** Extract GPS_FLAG_* bits from a packed advert flags byte. */
#define GPS_ADVERT_FLAGS_GET(b)       ((uint8_t)((b) & 0x3Fu))
/** Extract mobility class from a packed advert flags byte. */
#define GPS_ADVERT_MOBILITY_GET(b)    ((uint8_t)((uint8_t)(b) >> 6))

_Static_assert(sizeof(rivr_gps_advert_t) == 8u,
               "rivr_gps_advert_t must be 8 bytes");

/**
 * GPS_SUB_META — 14 bytes total.
 *
 * Sent once at boot for static nodes (repeater / gateway).
 * Contains precise fixed installation position.
 * TTL = 1.
 */
typedef struct __attribute__((packed)) {
    uint8_t  svc;         /**< RIVR_DATA_SVC_GPS                             */
    uint8_t  subtype;     /**< GPS_SUB_META                                  */
    uint8_t  flags;       /**< GPS_FLAG_* bitmask                            */
    uint8_t  pos_format;  /**< 0 = lat/lon e5 + alt_m (only format defined) */
    int32_t  lat_e5;      /**< Latitude  × 10⁻⁵ (degrees)                   */
    int32_t  lon_e5;      /**< Longitude × 10⁻⁵ (degrees)                   */
    int16_t  alt_m;       /**< Altitude in metres (–32768 = unknown)         */
} rivr_gps_meta_t;

_Static_assert(sizeof(rivr_gps_meta_t) == 14u,
               "rivr_gps_meta_t must be 14 bytes");

/**
 * GPS_SUB_POS_REQ — 4 bytes total.
 *
 * Unicast request sent to a specific node asking for its position.
 */
typedef struct __attribute__((packed)) {
    uint8_t svc;          /**< RIVR_DATA_SVC_GPS                             */
    uint8_t subtype;      /**< GPS_SUB_POS_REQ                               */
    uint8_t mode;         /**< 0 = coarse (ADVERT), 1 = fine (POS_RESP)     */
    uint8_t max_age_min;  /**< Accept cached fix up to this many minutes old */
} rivr_gps_pos_req_t;

_Static_assert(sizeof(rivr_gps_pos_req_t) == 4u,
               "rivr_gps_pos_req_t must be 4 bytes");

/**
 * GPS_SUB_POS_RESP — 12 bytes total.
 *
 * Unicast reply with fine position.  NOT flooded (TTL = 1, unicast dst).
 * Speed and course are internal-state fields only; they are not transmitted
 * in the wire frame to keep the struct at the 12-byte wire budget.
 */
typedef struct __attribute__((packed)) {
    uint8_t  svc;          /**< RIVR_DATA_SVC_GPS                            */
    uint8_t  subtype;      /**< GPS_SUB_POS_RESP                             */
    uint8_t  flags;        /**< GPS_FLAG_* bitmask                           */
    uint8_t  age_min;      /**< Minutes since fix was acquired (0 = fresh)   */
    int32_t  lat_e5;       /**< Latitude  × 10⁻⁵                            */
    int32_t  lon_e5;       /**< Longitude × 10⁻⁵                            */
} rivr_gps_pos_resp_t;

_Static_assert(sizeof(rivr_gps_pos_resp_t) == 12u,
               "rivr_gps_pos_resp_t must be 12 bytes");

/* ── Mobility classification ─────────────────────────────────────────────── */

/**
 * Mobility class assigned by gps_classify().
 *   STATIC  — stationary infrastructure or parked device
 *   SLOW    — pedestrian / cycling
 *   FAST    — vehicle / fast movement
 */
typedef enum {
    RIVR_MOB_STATIC = 0,
    RIVR_MOB_SLOW   = 1,
    RIVR_MOB_FAST   = 2
} rivr_mobility_t;

/* ── Node tracking cache ─────────────────────────────────────────────────── */

/** Maximum number of remote GPS nodes tracked simultaneously. */
#define GPS_NODE_CACHE_SIZE  16u

/**
 * One entry in the node position cache.
 *
 * position is stored in degrees × 10⁻⁵ (int32) so it matches POS_RESP.
 */
typedef struct {
    uint32_t  node_id;        /**< Node ID; 0 = empty slot                  */
    uint8_t   flags;          /**< GPS_FLAG_* from last ADVERT/META         */
    uint8_t   mobility;       /**< rivr_mobility_t                          */
    uint8_t   state_seq;      /**< Last received state_seq                  */
    uint8_t   pos_age_min;    /**< Approximate position age in minutes      */
    int32_t   lat_e5;         /**< Last known latitude × 10⁻⁵               */
    int32_t   lon_e5;         /**< Last known longitude × 10⁻⁵              */
    uint32_t  last_seen_ms;   /**< tb_millis() at last reception            */
} rivr_node_cache_t;

/* ── Local GPS state ─────────────────────────────────────────────────────── */

/** Tracking state machine values for mobile nodes. */
typedef enum {
    GPS_STATE_NO_FIX       = 0,  /**< No GNSS lock yet                      */
    GPS_STATE_FIX_ACQUIRE  = 1,  /**< Lock acquired, waiting for stability  */
    GPS_STATE_TRACK_IDLE   = 2,  /**< Fix valid, no significant motion       */
    GPS_STATE_TRACK_MOVING = 3,  /**< Slow / pedestrian motion              */
    GPS_STATE_TRACK_FAST   = 4   /**< Fast (vehicle) motion                 */
} gps_track_state_t;

/**
 * Complete local GPS state.  Stored in BSS (zero-initialised).
 */
typedef struct {
    /* ── Current fix ─────────────────────────────────────────────────────── */
    int32_t   lat_e5;             /**< Filtered latitude × 10⁻⁵             */
    int32_t   lon_e5;             /**< Filtered longitude × 10⁻⁵            */
    int16_t   alt_m;              /**< Altitude in metres                    */
    uint8_t   speed_kmh;          /**< Speed rounded to km/h                */
    uint8_t   course_deg2;        /**< Course / 2 (0xFF = unknown)          */
    bool      fix_valid;          /**< True when fix passes all filters      */

    /* ── State machine ───────────────────────────────────────────────────── */
    gps_track_state_t track_state;
    uint8_t   state_seq;          /**< Wrapping 8-bit counter per state chg */
    rivr_mobility_t   mobility;

    /* ── Publish thresholds ──────────────────────────────────────────────── */
    int32_t   last_pub_lat_e5;    /**< Position at last publish              */
    int32_t   last_pub_lon_e5;
    uint16_t  last_pub_course;    /**< Course × 2 at last publish            */
    uint32_t  last_pub_ms;        /**< tb_millis() at last publish           */
    uint32_t  last_advert_ms;     /**< tb_millis() at last ADVERT TX        */

    /* ── Meta tracking (static nodes) ───────────────────────────────────── */
    bool      meta_sent;          /**< True after first META has been queued */

    /* ── Remote node cache ───────────────────────────────────────────────── */
    rivr_node_cache_t cache[GPS_NODE_CACHE_SIZE];
} gps_state_t;

/* ── TX configuration ────────────────────────────────────────────────────── */

/** ADVERT interval for static nodes: 30 min base + up to 30 min jitter. */
#define GPS_STATIC_ADVERT_INTERVAL_MS   (30u * 60u * 1000u)
#define GPS_STATIC_ADVERT_JITTER_MS     (30u * 60u * 1000u)

/** Timeout before a mobile node sends an ADVERT even without movement. */
#define GPS_MOBILE_TIMEOUT_MS           (20u * 60u * 1000u)

/** Minimum distance to trigger a new publish (metres × 10 to avoid float). */
#define GPS_PUBLISH_DIST_M10            1500u   /**< 150 m × 10              */

/** Heading change that triggers a publish (degrees). */
#define GPS_PUBLISH_HEADING_DEG         35u

/** Spike rejection: max believable speed jump between two fixes (km/h). */
/** Speed spike rejection threshold (km/h).
 *  speed_kmh is uint8_t (max 255); 250 km/h is the highest representable
 *  value that is still a meaningful upper bound for terrestrial devices. */
#define GPS_SPIKE_SPEED_KMH             250u

/** Mobility thresholds. */
#define GPS_MOB_SLOW_THRESH_KMH         2u    /**< Below → STATIC           */
#define GPS_MOB_FAST_THRESH_KMH         15u   /**< Above → FAST             */
#define GPS_MOB_STATIC_DIST_M10         300u  /**< <30 m in 5 min → STATIC  */

/** Node cache expiry: entries older than this are considered stale. */
#define GPS_CACHE_EXPIRY_MS             (2u * 60u * 60u * 1000u)  /**< 2 h  */

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Initialise the GPS service state.
 *
 * Must be called once at boot, before any other gps_ function.
 * For static nodes, pass @p static_lat_e5 / @p static_lon_e5 as the
 * pre-configured installation position; both 0 means position unknown.
 *
 * @param is_infra       True for repeaters and gateways.
 * @param static_lat_e5  Pre-configured latitude  × 10⁻⁵ (0 if mobile/unknown).
 * @param static_lon_e5  Pre-configured longitude × 10⁻⁵ (0 if mobile/unknown).
 * @param static_alt_m   Altitude in metres (–32768 if unknown).
 */
void gps_init(bool is_infra,
              int32_t static_lat_e5,
              int32_t static_lon_e5,
              int16_t static_alt_m);

/**
 * @brief Feed a new raw GNSS fix to the GPS service.
 *
 * Applies spike rejection, low-pass filter, and movement detection.
 * Does NOT transmit anything — call gps_tick() from the main loop.
 *
 * @param lat_e5     Latitude  × 10⁻⁵ from GNSS parser.
 * @param lon_e5     Longitude × 10⁻⁵ from GNSS parser.
 * @param alt_m      Altitude in metres (INT16_MIN if unavailable).
 * @param speed_kmh  Speed in km/h (rounded).
 * @param course_deg Course 0–359 degrees (0xFFFF if unavailable).
 * @param valid      True if GNSS reports a valid 2D/3D fix.
 */
void gps_feed_fix(int32_t lat_e5,
                  int32_t lon_e5,
                  int16_t alt_m,
                  uint8_t speed_kmh,
                  uint16_t course_deg,
                  bool valid);

/**
 * @brief Periodic tick — call once per main-loop iteration.
 *
 * Evaluates timers, decides whether to queue an ADVERT, META, or any deferred
 * position publish.  Calls gps_tx_advert() / gps_tx_meta() internally.
 *
 * @param now_ms  Current monotonic time from tb_millis().
 */
void gps_tick(uint32_t now_ms);

/**
 * @brief Dispatch an incoming PKT_DATA frame to the GPS service.
 *
 * Called from the application service dispatch in rivr_sources.c.
 * Silently returns if payload is not a GPS frame.
 *
 * @param src_id      Source node ID from the packet header.
 * @param dst_id      Destination node ID (0 = broadcast).
 * @param payload     Pointer to PKT_DATA payload bytes.
 * @param len         Payload length in bytes.
 * @param now_ms      Current monotonic time.
 */
void gps_on_rx(uint32_t src_id,
               uint32_t dst_id,
               const uint8_t *payload,
               uint8_t len,
               uint32_t now_ms);

/**
 * @brief Classify mobility based on speed and recent displacement.
 *
 * @param speed_kmh       Current speed in km/h.
 * @param dist_5min_m10   Displacement over ~5 min (metres × 10).
 * @return rivr_mobility_t
 */
rivr_mobility_t gps_classify(uint8_t speed_kmh, uint32_t dist_5min_m10);

/**
 * @brief Determine whether a new position publication is warranted.
 *
 * Checks distance moved, heading change, and periodic-timeout triggers.
 *
 * @param now_ms           Current monotonic time.
 * @param new_lat_e5       New filtered latitude.
 * @param new_lon_e5       New filtered longitude.
 * @param new_course_deg2  New course / 2 (0xFF = unknown).
 * @return true if a new ADVERT/position should be transmitted.
 */
bool gps_should_publish(uint32_t now_ms,
                        int32_t  new_lat_e5,
                        int32_t  new_lon_e5,
                        uint8_t  new_course_deg2);

/**
 * @brief Look up a node in the remote position cache.
 *
 * @param node_id  Node to look up.
 * @return Pointer to a cache entry (may be stale — check last_seen_ms), or
 *         NULL if node is not in cache.
 */
const rivr_node_cache_t *gps_cache_get(uint32_t node_id);

/**
 * @brief Iterate over all valid (non-expired) cache entries.
 *
 * idx ranges 0..GPS_NODE_CACHE_SIZE-1; returns NULL for empty/expired slots.
 */
const rivr_node_cache_t *gps_cache_at(uint8_t idx, uint32_t now_ms);

/**
 * @brief Send a POS_REQ unicast to a specific node.
 *
 * Queues a unicast PKT_DATA/GPS_SUB_POS_REQ frame.
 *
 * @param dst_id      Target node ID.
 * @param mode        0 = coarse, 1 = fine.
 * @param max_age_min Accept reply if fix age ≤ this many minutes.
 */
void gps_request_position(uint32_t dst_id, uint8_t mode, uint8_t max_age_min);

/**
 * @brief Update the node's own fixed position at runtime.
 *
 * Safe to call from CLI or BLE companion handler at any time (single-core
 * ESP32 main-loop context only).  Also queues an immediate META + ADVERT
 * so neighbours learn about the position without waiting for the next tick.
 *
 * @param lat_e5   Latitude  × 10⁻⁵.
 * @param lon_e5   Longitude × 10⁻⁵.
 * @param alt_m    Altitude in metres (INT16_MIN = unknown).
 */
void gps_set_position(int32_t lat_e5, int32_t lon_e5, int16_t alt_m);

/**
 * @brief Read-only access to the local GPS state (for diagnostics / CLI).
 */
const gps_state_t *gps_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_GPS_H */
