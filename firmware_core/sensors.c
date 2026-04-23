/**
 * @file  sensors.c
 * @brief DS18B20 + AM2302 + VBAT sensor subsystem — periodic PKT_TELEMETRY origination.
 *
 * All enabled sensors are sampled once per RIVR_SENSOR_TX_MS interval and
 * packed into a SINGLE PKT_TELEMETRY broadcast with N consecutive 11-byte
 * readings in the payload (payload_len = N × 11).  This minimises LoRa
 * airtime, duty-cycle usage and relay load versus the old approach of sending
 * one packet per sensor.
 *
 * Transmit schedule:
 *   T = 0 s  Trigger fires:
 *             • AM2302 read (blocking ~5 ms) — appended immediately.
 *             • VBAT ADC read (instant)      — appended immediately.
 *             • DS18B20 conversion started   — non-blocking, result in ~800 ms.
 *   T ≈ 0.8 s DS18B20 ready → append DS18B20 reading → send combined packet.
 *
 *   If DS18B20 is disabled the packet is sent at the trigger tick itself
 *   (no waiting required).
 *
 * Wire format: payload = [reading_0][reading_1]…[reading_N-1]
 *   Each reading is SVC_TELEMETRY_PAYLOAD_LEN (11) bytes:
 *   [0-1]  sensor_id  u16 LE   (1=DS18B20; 2=AM2302 RH; 3=AM2302 temp; 4=VBAT)
 *   [2-5]  value      i32 LE   (°C×100 / %RH×100 / mV)
 *   [6]    unit_code  u8       (UNIT_CELSIUS / UNIT_PERCENT_RH / UNIT_MILLIVOLTS)
 *   [7-10] timestamp  u32 LE   (seconds since boot)
 */

#include "sensors.h"
#include "sensor_ds18b20.h"
#include "sensor_am2302.h"
#include "sensor_vbat.h"
#include "protocol.h"
#include "rivr_layer/rivr_embed.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

/* Shared frame-sequence counter (defined in rivr_layer/rivr_embed.c).
 * Using the same counter avoids pkt_id collisions with CLI/beacon frames,
 * which would cause false deduplication at relay nodes.                 */
extern uint32_t g_ctrl_seq;

#define TAG "SENSORS"

/* ── Sensor ID assignments ──────────────────────────────────────────────── */
#define SENSOR_ID_DS18B20_TEMP  1u   /**< DS18B20 temperature  (UNIT_CELSIUS)    */
#define SENSOR_ID_AM2302_RH     2u   /**< AM2302 humidity      (UNIT_PERCENT_RH) */
#define SENSOR_ID_AM2302_TEMP   3u   /**< AM2302 temperature   (UNIT_CELSIUS)    */
#define SENSOR_ID_VBAT          4u   /**< Battery voltage      (UNIT_MILLIVOLTS) */

/* ── State ──────────────────────────────────────────────────────────────── */
#if RIVR_FEATURE_DS18B20
static ds18b20_ctx_t s_ds_ctx;
#endif

#if RIVR_FEATURE_AM2302
static am2302_ctx_t s_am_ctx;
#endif

/* VBAT is stateless after init — vbat_read_mv() is called directly. */

#if RIVR_FEATURE_DS18B20 || RIVR_FEATURE_AM2302 || RIVR_FEATURE_VBAT
/** Monotonic timestamp of last sensor TX trigger (in ms). */
static uint32_t s_last_trigger_ms = 0u;

/** Set after first sensors_tick() call so we can time the first trigger. */
static bool s_started = false;

/* ── Sensor bundle — collects all readings into one LoRa packet ─────────────
 *
 * All readings are packed consecutively:
 *   payload = [11-byte reading 0][11-byte reading 1] … [11-byte reading N-1]
 * payload_len = count × SVC_TELEMETRY_PAYLOAD_LEN.
 *
 * AM2302 + VBAT are appended at the trigger tick.
 * DS18B20 is appended ~800 ms later when the conversion finishes.
 * When all expected readings are present, send_bundle() transmits one packet.
 */
#define SENSOR_MAX_READINGS  4u
#define SENSOR_BUNDLE_LEN    (SENSOR_MAX_READINGS * SVC_TELEMETRY_PAYLOAD_LEN)

static uint8_t  s_bundle_buf[SENSOR_BUNDLE_LEN]; /**< Raw payload bytes          */
static uint8_t  s_bundle_count   = 0u;           /**< Readings collected so far  */
static uint32_t s_bundle_ts_s    = 0u;           /**< Shared bundle timestamp    */
static uint32_t s_bundle_node_id = 0u;           /**< Captured at trigger time   */
static uint16_t s_bundle_net_id  = 0u;           /**< Captured at trigger time   */

/** True after the trigger fires while waiting for DS18B20 conversion. */
static bool s_ds18b20_pending = false;
#endif /* any sensor feature */

#if RIVR_FEATURE_DS18B20 || RIVR_FEATURE_AM2302 || RIVR_FEATURE_VBAT
/* ── Internal helpers ───────────────────────────────────────────────────── */

/**
 * Append one 11-byte reading to the bundle buffer.
 */
static void bundle_append(uint16_t sensor_id, int32_t value, uint8_t unit_code)
{
    if (s_bundle_count >= SENSOR_MAX_READINGS) {
        ESP_LOGW(TAG, "bundle full — sensor_id=%u dropped", (unsigned)sensor_id);
        return;
    }
    uint8_t *p = s_bundle_buf + s_bundle_count * SVC_TELEMETRY_PAYLOAD_LEN;
    p[0]  = (uint8_t)(sensor_id & 0xFFu);
    p[1]  = (uint8_t)((sensor_id >> 8) & 0xFFu);
    p[2]  = (uint8_t)((uint32_t)value & 0xFFu);
    p[3]  = (uint8_t)(((uint32_t)value >>  8) & 0xFFu);
    p[4]  = (uint8_t)(((uint32_t)value >> 16) & 0xFFu);
    p[5]  = (uint8_t)(((uint32_t)value >> 24) & 0xFFu);
    p[6]  = unit_code;
    p[7]  = (uint8_t)(s_bundle_ts_s & 0xFFu);
    p[8]  = (uint8_t)((s_bundle_ts_s >>  8) & 0xFFu);
    p[9]  = (uint8_t)((s_bundle_ts_s >> 16) & 0xFFu);
    p[10] = (uint8_t)((s_bundle_ts_s >> 24) & 0xFFu);
    s_bundle_count++;
}

/**
 * Encode and transmit the bundle as a single PKT_TELEMETRY broadcast.
 * Resets the bundle state afterwards.
 */
static void send_bundle(void)
{
    if (s_bundle_count == 0u) return;

    uint8_t payload_len = (uint8_t)(s_bundle_count * SVC_TELEMETRY_PAYLOAD_LEN);

    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_TELEMETRY;
    hdr.flags       = 0u;
    hdr.ttl = 0u;   /* telemetry — no relay at all */    
    hdr.net_id      = s_bundle_net_id;
    hdr.src_id      = s_bundle_node_id;
    hdr.dst_id      = 0u;  /* broadcast */
    hdr.seq         = (uint16_t)++g_ctrl_seq;
    hdr.pkt_id      = (uint16_t)g_ctrl_seq;
    hdr.payload_len = payload_len;

    uint8_t buf[RIVR_PKT_HDR_LEN + SENSOR_BUNDLE_LEN + 2u /* CRC */];
    int enc = protocol_encode(&hdr, s_bundle_buf, payload_len,
                              buf, (uint8_t)sizeof(buf));
    if (enc <= 0) {
        ESP_LOGW(TAG, "protocol_encode failed (payload_len=%u)", (unsigned)payload_len);
    } else {
        /* Inject the fully-encoded packet into the RIVR engine so the
         * telemetry_tx pipeline can apply duty-cycle gating before TX. */
        rivr_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.stamp.clock    = 0u;  /* mono clock */
        ev.stamp.tick     = (uint64_t)s_bundle_ts_s * 1000u;
        ev.v.tag          = RIVR_VAL_BYTES;
        ev.v.as_bytes.len = (uint16_t)enc;
        memcpy(ev.v.as_bytes.buf, buf, (size_t)enc);
        rivr_result_t rc  = rivr_inject_event("telemetry_tx", &ev);
        if (rc.code != RIVR_OK) {
            ESP_LOGW(TAG, "rivr_inject_event(telemetry_tx) failed (code=%u)",
                     (unsigned)rc.code);
        } else {
            ESP_LOGI(TAG, "injected telemetry bundle (%u readings, %u bytes payload)",
                     (unsigned)s_bundle_count, (unsigned)payload_len);
        }
    }

    /* Reset bundle for next trigger */
    s_bundle_count   = 0u;
    s_ds18b20_pending = false;
}
#endif /* any sensor feature */

/* ── Public API ─────────────────────────────────────────────────────────── */

void sensors_init(void)
{
#if RIVR_FEATURE_DS18B20
    ds18b20_init(&s_ds_ctx, PIN_DS18B20_ONEWIRE);
#endif
#if RIVR_FEATURE_AM2302
    am2302_init(&s_am_ctx, PIN_AM2302_DATA);
#endif
#if RIVR_FEATURE_VBAT
    vbat_init(PIN_ADC_VBAT, RIVR_VBAT_DIV_NUM, RIVR_VBAT_DIV_DEN);
#endif
#if RIVR_FEATURE_DS18B20 || RIVR_FEATURE_AM2302 || RIVR_FEATURE_VBAT
    ESP_LOGI(TAG,
             "sensors_init OK (DS18B20=%d AM2302=%d VBAT=%d)",
             RIVR_FEATURE_DS18B20, RIVR_FEATURE_AM2302, RIVR_FEATURE_VBAT);
#endif
}

void sensors_tick(uint32_t now_ms, uint32_t node_id, uint16_t net_id)
{
#if !RIVR_FEATURE_DS18B20 && !RIVR_FEATURE_AM2302 && !RIVR_FEATURE_VBAT
    (void)now_ms; (void)node_id; (void)net_id;
    return;
#else
    /* Record time of first call so we can measure elapsed time consistently. */
    if (!s_started) {
        s_started = true;
        s_last_trigger_ms = now_ms;
    }

    /* ── Periodic trigger ───────────────────────────────────────────────── */
    if ((now_ms - s_last_trigger_ms) >= RIVR_SENSOR_TX_MS) {
        s_last_trigger_ms = now_ms;

        /* Reset bundle and record context for send_bundle() */
        s_bundle_count   = 0u;
        s_bundle_ts_s    = now_ms / 1000u;
        s_bundle_node_id = node_id;
        s_bundle_net_id  = net_id;
        s_ds18b20_pending = false;

#if RIVR_FEATURE_AM2302
        /* am2302_tick() blocks ~5 ms — acceptable at a ≥ 60 s interval. */
        if (am2302_tick(&s_am_ctx, now_ms)) {
            bundle_append(SENSOR_ID_AM2302_RH,
                          am2302_get_rh_x100(&s_am_ctx),
                          UNIT_PERCENT_RH);
            bundle_append(SENSOR_ID_AM2302_TEMP,
                          am2302_get_temp_x100(&s_am_ctx),
                          UNIT_CELSIUS);
        }
#endif

#if RIVR_FEATURE_VBAT
        {
            int32_t vbat_mv = vbat_read_mv();
            if (vbat_mv > 0) {
                bundle_append(SENSOR_ID_VBAT, vbat_mv, UNIT_MILLIVOLTS);
            }
        }
#endif

#if RIVR_FEATURE_DS18B20
        /* Start conversion — result arrives ~800 ms later via ds18b20_tick(). */
        if (ds18b20_start_conversion(&s_ds_ctx, now_ms)) {
            s_ds18b20_pending = true;
        } else {
            /* No sensor present or reset failed — send AM2302/VBAT immediately. */
            s_ds18b20_pending = false;
            send_bundle();
        }
#else
        /* No async sensor — send the bundle immediately. */
        send_bundle();
#endif
    }

    /* ── DS18B20 tick: advance conversion state machine ─────────────────── */
#if RIVR_FEATURE_DS18B20
    ds18b20_tick(&s_ds_ctx, now_ms);

    if (s_ds18b20_pending) {
        if (ds18b20_ready(&s_ds_ctx)) {
            bundle_append(SENSOR_ID_DS18B20_TEMP,
                          ds18b20_read_celsius_x100(&s_ds_ctx),
                          UNIT_CELSIUS);
            send_bundle();
        } else if (s_ds_ctx.state == DS18B20_STATE_ERROR) {
            /* Conversion or scratchpad readback failed — send without DS18B20. */
            ESP_LOGW(TAG, "DS18B20 error — sending bundle without temperature");
            send_bundle();
        }
    }
#endif

#endif /* at least one feature enabled */
}

