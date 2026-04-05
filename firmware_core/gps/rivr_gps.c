/**
 * @file  rivr_gps.c
 * @brief GPS + location-advertisement service implementation.
 *
 * Design principles
 * ─────────────────
 *  • All state in a single BSS-backed gps_state_t struct (no heap).
 *  • Fixed-point arithmetic throughout; no floating-point in hot path.
 *  • TX calls go directly to do_gps_tx() which encodes and pushes to
 *    rf_tx_queue via the same pattern as beacon_sink_cb / rivr_metrics.c.
 *  • gps_tick() is called from the main loop and drives all timers.
 *  • gps_on_rx() is called from the application service dispatch in
 *    rivr_sources.c for every PKT_DATA frame.
 */

#include "rivr_gps.h"
#include "../protocol.h"
#include "../timebase.h"
#include "../radio_sx1262.h"
#include "rivr_layer/rivr_embed.h"          /* g_my_node_id, g_net_id, g_ctrl_seq */
#include "../rivr_log.h"
#include <string.h>
#include <stdio.h>

/* ── Module tag ──────────────────────────────────────────────────────────── */
#define TAG "RIVR_GPS"

/* ── Module state (BSS) ─────────────────────────────────────────────────── */
static gps_state_t s_gps;

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * Approximate distance squared between two points in degrees×10⁻⁵.
 * Returns (Δlat² + Δlon²) * 100, which is proportional to distance² in m².
 *
 * Avoids sqrt: we compare directly against a threshold² to decide "moved".
 * At equator: 1e-5 deg ≈ 1.11 m, so 1 unit ≈ 1.11 m.
 * dist_m × 10 is what the callers use (GPS_PUBLISH_DIST_M10, etc.).
 *
 * Returns a value in (10⁻⁵ deg)² units — caller converts to m×10 equivalent.
 *
 * Overflow analysis: Δlat max = 18,000,000 (180°), Δlat² = 3.24×10¹⁴;
 * a uint64_t holds up to 1.8×10¹⁹ — safe.
 */
static uint32_t dist_approx_m10(int32_t lat1, int32_t lon1,
                                  int32_t lat2, int32_t lon2)
{
    int32_t dlat = lat2 - lat1;
    int32_t dlon = lon2 - lon1;
    /* Each unit is ~1.11 m; scale back to m×10 (multiply by 11, divide by 10) */
    /* Use uint32_t: max input delta ≈ 20,000 units for 200 km — 200,000²/11
     * stays within uint32_t.  For larger distances, saturate at UINT32_MAX.   */
    uint32_t adlat = (uint32_t)(dlat < 0 ? -dlat : dlat);
    uint32_t adlon = (uint32_t)(dlon < 0 ? -dlon : dlon);
    /* Cheap Chebyshev approximation: max(Δlat, Δlon) + 0.5×min(Δlat, Δlon)   *
     * Error: ≤ 8 % over-estimate — acceptable for publish thresholds.         */
    uint32_t hi = adlat > adlon ? adlat : adlon;
    uint32_t lo = adlat < adlon ? adlat : adlon;
    /* unit_m10 ≈ 11  (1.11 m/unit × 10)                                       */
    uint32_t approx_units = hi + lo / 2u;
    /* Saturate before multiply to avoid overflow on unrealistic inputs */
    if (approx_units > 390000u) return UINT32_MAX;
    return approx_units * 11u;   /* convert units → metres × 10 */
}

/**
 * Heading delta (0–180 degrees, always positive).
 */
static uint16_t heading_delta(uint16_t a_deg, uint16_t b_deg)
{
    uint16_t d = (a_deg >= b_deg) ? (a_deg - b_deg) : (b_deg - a_deg);
    if (d > 180u) d = (uint16_t)(360u - d);
    return d;
}

/**
 * Encode and push one GPS PKT_DATA frame onto rf_tx_queue.
 *
 * Uses the same pattern as do_beacon_tx() in rivr_sinks.c:
 *   • protocol_encode fills req.data[]
 *   • RF_TOA_APPROX_US computes airtime
 *   • rb_try_push enqueues or drops silently
 *
 * @param dst_id     0 = broadcast.
 * @param ttl        Usually 1 for GPS frames; 3 for POS_REQ.
 * @param payload    Pointer to the packed GPS struct.
 * @param payload_len Size of the GPS struct (must be ≤ RIVR_PKT_MAX_PAYLOAD).
 */
static void do_gps_tx(uint32_t    dst_id,
                      uint8_t     ttl,
                      const void *payload,
                      uint8_t     payload_len)
{
    if (payload_len == 0u || payload_len > RIVR_PKT_MAX_PAYLOAD) return;

    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_DATA;
    hdr.flags       = 0u;
    hdr.ttl         = ttl;
    hdr.hop         = 0u;
    hdr.net_id      = g_net_id;
    hdr.src_id      = g_my_node_id;
    hdr.dst_id      = dst_id;
    hdr.seq         = (uint16_t)++g_ctrl_seq;
    hdr.pkt_id      = (uint16_t)g_ctrl_seq;
    hdr.payload_len = payload_len;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    int enc = protocol_encode(&hdr, (const uint8_t *)payload, payload_len,
                               req.data, sizeof(req.data));
    if (enc <= 0) {
        RIVR_LOGW(TAG, "gps: encode failed (sub=0x%02x)", ((const uint8_t *)payload)[1]);
        return;
    }
    req.len    = (uint8_t)enc;
    req.toa_us = RF_TOA_APPROX_US(req.len);
    req.due_ms = 0u;

    if (!rb_try_push(&rf_tx_queue, &req)) {
        RIVR_LOGD(TAG, "gps: tx queue full, dropped sub=0x%02x",
                  ((const uint8_t *)payload)[1]);
    }
}

/* ── Cache helpers ───────────────────────────────────────────────────────── */

static rivr_node_cache_t *cache_find_or_alloc(uint32_t node_id, uint32_t now_ms)
{
    rivr_node_cache_t *oldest = NULL;
    uint32_t oldest_age = 0u;

    for (uint8_t i = 0u; i < GPS_NODE_CACHE_SIZE; i++) {
        rivr_node_cache_t *e = &s_gps.cache[i];

        if (e->node_id == node_id) return e;   /* found */

        if (e->node_id == 0u) return e;        /* empty slot */

        /* Track oldest for eviction */
        uint32_t age = now_ms - e->last_seen_ms;
        if (oldest == NULL || age > oldest_age) {
            oldest     = e;
            oldest_age = age;
        }
    }

    /* Evict oldest entry */
    if (oldest != NULL) {
        memset(oldest, 0, sizeof(*oldest));
        return oldest;
    }
    return NULL;
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

void gps_init(bool    is_infra,
              int32_t static_lat_e5,
              int32_t static_lon_e5,
              int16_t static_alt_m)
{
    memset(&s_gps, 0, sizeof(s_gps));

    if (is_infra) {
        /* Static node: pre-load known position and mark flags */
        s_gps.lat_e5      = static_lat_e5;
        s_gps.lon_e5      = static_lon_e5;
        s_gps.alt_m       = static_alt_m;
        s_gps.mobility    = RIVR_MOB_STATIC;
        s_gps.track_state = GPS_STATE_TRACK_IDLE;
        /* Mark fix valid only if a real position was provided */
        s_gps.fix_valid   = (static_lat_e5 != 0 || static_lon_e5 != 0);
    } else {
        s_gps.track_state = GPS_STATE_NO_FIX;
    }

    /* course 0xFF*2 = 510 → unknown sentinel */
    s_gps.course_deg2     = 0xFFu;
    s_gps.last_pub_course = 0xFFFFu;

    RIVR_LOGI(TAG, "gps_init: infra=%d fix=%d lat=%ld lon=%ld",
              (int)is_infra, (int)s_gps.fix_valid,
              (long)static_lat_e5, (long)static_lon_e5);
}

/* ── Fix ingestion + filtering ───────────────────────────────────────────── */

void gps_feed_fix(int32_t  lat_e5,
                  int32_t  lon_e5,
                  int16_t  alt_m,
                  uint8_t  speed_kmh,
                  uint16_t course_deg,
                  bool     valid)
{
    if (!valid) {
        s_gps.fix_valid = false;
        if (s_gps.track_state != GPS_STATE_NO_FIX &&
            s_gps.track_state != GPS_STATE_FIX_ACQUIRE) {
            s_gps.track_state = GPS_STATE_NO_FIX;
        }
        return;
    }

    /* ── Plausibility: reject fixes outside valid range ────────────────── */
    if (lat_e5 < -9000000L || lat_e5 > 9000000L) return;
    if (lon_e5 < -18000000L || lon_e5 > 18000000L) return;

    /* ── Spike rejection: unrealistic speed ─────────────────────────────── */
    if (s_gps.fix_valid) {
        uint32_t d10 = dist_approx_m10(s_gps.lat_e5, s_gps.lon_e5,
                                        lat_e5, lon_e5);
        /* d10 is metres×10; if > spike threshold m/s ... we just check
         * reported speed since we have no inter-fix timestamp here.         */
        if (speed_kmh > GPS_SPIKE_SPEED_KMH) {
            RIVR_LOGW(TAG, "gps spike: speed=%u kmh, ignored", speed_kmh);
            return;
        }
        /* Extra: large positional jump with zero reported speed → reject. */
        if (speed_kmh == 0u && d10 > 5000u) {  /* > 500 m jump at 0 km/h */
            RIVR_LOGW(TAG, "gps spike: standstill jump %lu m/10", (unsigned long)d10);
            return;
        }
    }

    /* ── Low-pass filter: α = 1/4 (0.75 old + 0.25 new) ────────────────── */
    if (s_gps.fix_valid) {
        s_gps.lat_e5 = (int32_t)(((int64_t)s_gps.lat_e5 * 3 + lat_e5) / 4);
        s_gps.lon_e5 = (int32_t)(((int64_t)s_gps.lon_e5 * 3 + lon_e5) / 4);
    } else {
        /* First fix: accept immediately */
        s_gps.lat_e5 = lat_e5;
        s_gps.lon_e5 = lon_e5;
    }
    s_gps.alt_m       = alt_m;
    s_gps.speed_kmh   = speed_kmh;
    s_gps.course_deg2 = (course_deg <= 359u) ? (uint8_t)(course_deg / 2u) : 0xFFu;
    s_gps.fix_valid   = true;

    /* ── State machine transition ─────────────────────────────────────────── */
    if (s_gps.track_state == GPS_STATE_NO_FIX) {
        s_gps.track_state = GPS_STATE_FIX_ACQUIRE;
        s_gps.state_seq++;
        RIVR_LOGI(TAG, "gps: first fix acquired lat=%ld lon=%ld",
                  (long)s_gps.lat_e5, (long)s_gps.lon_e5);
    } else if (s_gps.track_state == GPS_STATE_FIX_ACQUIRE) {
        /* Promote to IDLE once fix is stable (just use first valid fix) */
        s_gps.track_state = GPS_STATE_TRACK_IDLE;
        s_gps.state_seq++;
    }
}

/* ── Mobility classification ─────────────────────────────────────────────── */

rivr_mobility_t gps_classify(uint8_t speed_kmh, uint32_t dist_5min_m10)
{
    if (speed_kmh < GPS_MOB_SLOW_THRESH_KMH &&
        dist_5min_m10 < GPS_MOB_STATIC_DIST_M10) {
        return RIVR_MOB_STATIC;
    }
    if (speed_kmh >= GPS_MOB_FAST_THRESH_KMH) {
        return RIVR_MOB_FAST;
    }
    return RIVR_MOB_SLOW;
}

/* ── Publish decision ────────────────────────────────────────────────────── */

bool gps_should_publish(uint32_t now_ms,
                        int32_t  new_lat_e5,
                        int32_t  new_lon_e5,
                        uint8_t  new_course_deg2)
{
    /* Always publish immediately after first fix */
    if (s_gps.last_pub_ms == 0u) return true;

    /* Distance trigger */
    uint32_t d10 = dist_approx_m10(s_gps.last_pub_lat_e5, s_gps.last_pub_lon_e5,
                                    new_lat_e5, new_lon_e5);
    if (d10 >= GPS_PUBLISH_DIST_M10) return true;

    /* Heading trigger — only meaningful for mobile nodes */
    if (s_gps.last_pub_course != 0xFFFFu && new_course_deg2 != 0xFFu) {
        uint16_t old_deg = (uint16_t)s_gps.last_pub_course * 2u;
        uint16_t new_deg = (uint16_t)new_course_deg2 * 2u;
        if (heading_delta(old_deg, new_deg) >= GPS_PUBLISH_HEADING_DEG) return true;
    }

    /* Periodic timeout */
    if ((now_ms - s_gps.last_pub_ms) >= GPS_MOBILE_TIMEOUT_MS) return true;

    return false;
}

/* ── TX helpers ──────────────────────────────────────────────────────────── */

/**
 * Build and queue a GPS_SUB_ADVERT frame.
 * TTL = 1 (neighbourhood-only; no mesh flood).
 */
static void gps_tx_advert(uint32_t now_ms)
{
    uint8_t flags = 0u;
    if (s_gps.fix_valid)                      flags |= GPS_FLAG_POS_VALID;
    if (s_gps.mobility == RIVR_MOB_STATIC)    flags |= GPS_FLAG_INFRA;   /* infra hint */

    rivr_gps_advert_t pkt;
    pkt.svc       = RIVR_DATA_SVC_GPS;
    pkt.subtype   = GPS_SUB_ADVERT;
    pkt.flags     = GPS_ADVERT_FLAGS_PACK(flags, s_gps.mobility);
    pkt.state_seq = s_gps.state_seq;
    pkt.lat_q     = GPS_LAT_Q_FROM_E5(s_gps.lat_e5);
    pkt.lon_q     = GPS_LON_Q_FROM_E5(s_gps.lon_e5);

    do_gps_tx(0u /*broadcast*/, 1u /*TTL*/, &pkt, sizeof(pkt));

    s_gps.last_advert_ms    = now_ms;
    s_gps.last_pub_ms       = now_ms;
    s_gps.last_pub_lat_e5   = s_gps.lat_e5;
    s_gps.last_pub_lon_e5   = s_gps.lon_e5;
    s_gps.last_pub_course   = (uint16_t)s_gps.course_deg2;

    RIVR_LOGI(TAG, "advert TX: mob=%u lat_q=%d lon_q=%d seq=%u",
              (unsigned)s_gps.mobility,
              (int)pkt.lat_q, (int)pkt.lon_q,
              (unsigned)s_gps.state_seq);
}

/**
 * Build and queue a GPS_SUB_META frame (static nodes only, once at boot).
 * TTL = 1.
 */
static void gps_tx_meta(void)
{
    if (s_gps.meta_sent) return;   /* Send only once */

    rivr_gps_meta_t pkt;
    pkt.svc        = RIVR_DATA_SVC_GPS;
    pkt.subtype    = GPS_SUB_META;
    pkt.flags      = GPS_FLAG_INFRA | (s_gps.fix_valid ? GPS_FLAG_POS_VALID : 0u);
    pkt.pos_format = 0u;
    pkt.lat_e5     = s_gps.lat_e5;
    pkt.lon_e5     = s_gps.lon_e5;
    pkt.alt_m      = s_gps.alt_m;

    do_gps_tx(0u /*broadcast*/, 1u /*TTL*/, &pkt, sizeof(pkt));

    s_gps.meta_sent = true;
    RIVR_LOGI(TAG, "META TX: lat=%ld lon=%ld alt=%d",
              (long)pkt.lat_e5, (long)pkt.lon_e5, (int)pkt.alt_m);
}

/**
 * Build and queue a GPS_SUB_POS_RESP unicast reply.
 * TTL = 1, unicast to requester.
 */
static void gps_tx_pos_resp(uint32_t dst_id, uint8_t age_min)
{
    rivr_gps_pos_resp_t pkt;
    pkt.svc         = RIVR_DATA_SVC_GPS;
    pkt.subtype     = GPS_SUB_POS_RESP;
    pkt.flags       = (s_gps.fix_valid ? GPS_FLAG_POS_VALID : 0u)
                    | (s_gps.mobility == RIVR_MOB_STATIC ? GPS_FLAG_INFRA : 0u);
    pkt.age_min     = age_min;
    pkt.lat_e5      = s_gps.lat_e5;
    pkt.lon_e5      = s_gps.lon_e5;

    /* TTL = 1 unicast — POS_RESP must not flood the mesh */
    do_gps_tx(dst_id, 1u, &pkt, sizeof(pkt));

    RIVR_LOGI(TAG, "POS_RESP TX to 0x%08lx: lat=%ld lon=%ld",
              (unsigned long)dst_id, (long)pkt.lat_e5, (long)pkt.lon_e5);
}

/* ── Periodic tick ───────────────────────────────────────────────────────── */

void gps_tick(uint32_t now_ms)
{
    /* ── Static / infrastructure nodes ─────────────────────────────────── */
    if (s_gps.mobility == RIVR_MOB_STATIC) {
        /* Send META once at boot */
        if (!s_gps.meta_sent) {
            gps_tx_meta();
            gps_tx_advert(now_ms);
            return;
        }

        /* Periodic ADVERT (30–60 min with jitter seeded by node_id) */
        uint32_t jitter = (g_my_node_id & 0xFFFFu)
                          % (GPS_STATIC_ADVERT_JITTER_MS + 1u);
        uint32_t interval = GPS_STATIC_ADVERT_INTERVAL_MS + jitter;
        if (s_gps.last_advert_ms == 0u ||
            (now_ms - s_gps.last_advert_ms) >= interval) {
            gps_tx_advert(now_ms);
        }
        return;
    }

    /* ── Mobile nodes ────────────────────────────────────────────────────── */
    if (!s_gps.fix_valid) return;

    /* First fix just acquired → immediate send */
    if (s_gps.track_state == GPS_STATE_FIX_ACQUIRE) {
        s_gps.track_state = GPS_STATE_TRACK_IDLE;
        gps_tx_advert(now_ms);
        return;
    }

    /* Event-driven publish */
    if (s_gps.track_state >= GPS_STATE_TRACK_IDLE) {
        if (gps_should_publish(now_ms, s_gps.lat_e5, s_gps.lon_e5,
                               s_gps.course_deg2)) {
            /* Update tracking state before TX */
            rivr_mobility_t mob = gps_classify(s_gps.speed_kmh, 0u /*dist_5min not tracked here*/);
            if (mob != s_gps.mobility) {
                s_gps.mobility = mob;
                s_gps.state_seq++;
                /* Update track state */
                if (mob == RIVR_MOB_FAST)   s_gps.track_state = GPS_STATE_TRACK_FAST;
                else if (mob == RIVR_MOB_SLOW) s_gps.track_state = GPS_STATE_TRACK_MOVING;
                else                           s_gps.track_state = GPS_STATE_TRACK_IDLE;
            }
            gps_tx_advert(now_ms);
        }
    }
}

/* ── RX dispatch ─────────────────────────────────────────────────────────── */

void gps_on_rx(uint32_t       src_id,
               uint32_t       dst_id,
               const uint8_t *payload,
               uint8_t        len,
               uint32_t       now_ms)
{
    if (src_id == 0u || payload == NULL || len < 2u) return;

    /* Not a GPS frame — silently ignore */
    if (payload[0] != RIVR_DATA_SVC_GPS) return;

    uint8_t subtype = payload[1];

    switch (subtype) {
    /* ── ADVERT ──────────────────────────────────────────────────────────── */
    case GPS_SUB_ADVERT: {
        if (len < sizeof(rivr_gps_advert_t)) return;
        const rivr_gps_advert_t *a = (const rivr_gps_advert_t *)payload;

        rivr_node_cache_t *entry = cache_find_or_alloc(src_id, now_ms);
        if (!entry) return;

        bool state_changed = (entry->node_id == 0u ||
                              entry->state_seq != a->state_seq);

        entry->node_id      = src_id;
        entry->flags        = GPS_ADVERT_FLAGS_GET(a->flags);
        entry->mobility     = GPS_ADVERT_MOBILITY_GET(a->flags);
        entry->state_seq    = a->state_seq;
        entry->last_seen_ms = now_ms;
        /* Decode coarse position to e5 for storage */
        entry->lat_e5       = GPS_LAT_Q_TO_E5(a->lat_q);
        entry->lon_e5       = GPS_LON_Q_TO_E5(a->lon_q);
        /* pos_age_min stays 0 — coarse fixes are considered "current" */
        entry->pos_age_min  = 0u;

        /* Emit structured JSON line for host tool / BLE forwarding */
        printf("@GPS {\"type\":\"advert\",\"src\":\"0x%08lx\",\"mob\":%u,"
               "\"seq\":%u,\"lat_q\":%d,\"lon_q\":%d}\r\n",
               (unsigned long)src_id, (unsigned)GPS_ADVERT_MOBILITY_GET(a->flags),
               (unsigned)a->state_seq, (int)a->lat_q, (int)a->lon_q);

        /* If state_seq changed, request fine position */
        if (state_changed && src_id != g_my_node_id) {
            gps_request_position(src_id, 1u /*fine*/, 5u /*accept up to 5 min old*/);
        }
        break;
    }

    /* ── META ────────────────────────────────────────────────────────────── */
    case GPS_SUB_META: {
        if (len < sizeof(rivr_gps_meta_t)) return;
        const rivr_gps_meta_t *m = (const rivr_gps_meta_t *)payload;

        rivr_node_cache_t *entry = cache_find_or_alloc(src_id, now_ms);
        if (!entry) return;

        entry->node_id      = src_id;
        entry->flags        = m->flags;
        entry->mobility     = RIVR_MOB_STATIC;
        entry->last_seen_ms = now_ms;
        entry->lat_e5       = m->lat_e5;
        entry->lon_e5       = m->lon_e5;
        entry->pos_age_min  = 0u;

        printf("@GPS {\"type\":\"meta\",\"src\":\"0x%08lx\","
               "\"lat_e5\":%ld,\"lon_e5\":%ld,\"alt_m\":%d}\r\n",
               (unsigned long)src_id, (long)m->lat_e5,
               (long)m->lon_e5, (int)m->alt_m);
        break;
    }

    /* ── POS_REQ ─────────────────────────────────────────────────────────── */
    case GPS_SUB_POS_REQ: {
        if (len < sizeof(rivr_gps_pos_req_t)) return;
        /* Only respond if the request is addressed to us */
        if (dst_id != g_my_node_id && dst_id != 0u) return;
        if (!s_gps.fix_valid) return;

        const rivr_gps_pos_req_t *r = (const rivr_gps_pos_req_t *)payload;

        /* Compute fix age in minutes */
        uint32_t age_ms  = now_ms - s_gps.last_pub_ms;
        uint8_t  age_min = (uint8_t)(age_ms / 60000u);
        if (age_min > r->max_age_min) {
            RIVR_LOGD(TAG, "POS_REQ from 0x%08lx: fix too old (%u min)",
                      (unsigned long)src_id, (unsigned)age_min);
            return;
        }

        gps_tx_pos_resp(src_id, age_min);
        break;
    }

    /* ── POS_RESP ────────────────────────────────────────────────────────── */
    case GPS_SUB_POS_RESP: {
        if (len < sizeof(rivr_gps_pos_resp_t)) return;
        const rivr_gps_pos_resp_t *rsp = (const rivr_gps_pos_resp_t *)payload;

        rivr_node_cache_t *entry = cache_find_or_alloc(src_id, now_ms);
        if (!entry) return;

        entry->node_id      = src_id;
        entry->flags        = rsp->flags;
        entry->last_seen_ms = now_ms;
        entry->lat_e5       = rsp->lat_e5;
        entry->lon_e5       = rsp->lon_e5;
        entry->pos_age_min  = rsp->age_min;

        printf("@GPS {\"type\":\"pos_resp\",\"src\":\"0x%08lx\","
               "\"lat_e5\":%ld,\"lon_e5\":%ld,\"age\":%u}\r\n",
               (unsigned long)src_id, (long)rsp->lat_e5, (long)rsp->lon_e5,
               (unsigned)rsp->age_min);
        break;
    }

    default:
        /* Unknown subtype — ignore */
        break;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void gps_request_position(uint32_t dst_id, uint8_t mode, uint8_t max_age_min)
{
    if (dst_id == 0u || dst_id == g_my_node_id) return;

    rivr_gps_pos_req_t pkt;
    pkt.svc         = RIVR_DATA_SVC_GPS;
    pkt.subtype     = GPS_SUB_POS_REQ;
    pkt.mode        = mode;
    pkt.max_age_min = max_age_min;

    /* TTL = 3: allow up to 3 hops for unicast requests */
    do_gps_tx(dst_id, 3u, &pkt, sizeof(pkt));

    RIVR_LOGD(TAG, "POS_REQ → 0x%08lx mode=%u max_age=%u",
              (unsigned long)dst_id, (unsigned)mode, (unsigned)max_age_min);
}

const rivr_node_cache_t *gps_cache_get(uint32_t node_id)
{
    for (uint8_t i = 0u; i < GPS_NODE_CACHE_SIZE; i++) {
        if (s_gps.cache[i].node_id == node_id) {
            return &s_gps.cache[i];
        }
    }
    return NULL;
}

const rivr_node_cache_t *gps_cache_at(uint8_t idx, uint32_t now_ms)
{
    if (idx >= GPS_NODE_CACHE_SIZE) return NULL;
    const rivr_node_cache_t *e = &s_gps.cache[idx];
    if (e->node_id == 0u) return NULL;
    if ((now_ms - e->last_seen_ms) > GPS_CACHE_EXPIRY_MS) return NULL;
    return e;
}

const gps_state_t *gps_get_state(void)
{
    return &s_gps;
}

void gps_set_position(int32_t lat_e5, int32_t lon_e5, int16_t alt_m)
{
    s_gps.lat_e5    = lat_e5;
    s_gps.lon_e5    = lon_e5;
    s_gps.alt_m     = alt_m;
    s_gps.fix_valid = true;
    s_gps.meta_sent = false;  /* re-broadcast META to neighbours */

    if (s_gps.mobility == RIVR_MOB_STATIC) {
        /* Force immediate TX on next gps_tick() call */
        s_gps.last_advert_ms = 0u;
    }

    RIVR_LOGI(TAG, "gps_set_position: lat=%ld lon=%ld alt=%d",
              (long)lat_e5, (long)lon_e5, (int)alt_m);
}
