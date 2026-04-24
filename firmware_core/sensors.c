/**
 * @file  sensors.c
 * @brief DS18B20 + AM2302 + VBAT sensor subsystem — hybrid delta + heartbeat TX.
 *
 * All enabled sensors are sampled once per tick and packed into a SINGLE
 * PKT_TELEMETRY broadcast with N consecutive 11-byte readings in the payload
 * (payload_len = N × 11).  This minimises LoRa airtime versus one packet
 * per sensor.
 *
 * Transmit policy (hybrid: delta-on-change + heartbeat):
 *   A packet is sent when ANY of these is true:
 *     1. A reading changed by more than its delta threshold since the last TX
 *        AND at least RIVR_SENSOR_MIN_DELTA_MS has elapsed since the last TX.
 *     2. RIVR_SENSOR_TX_MS has elapsed since the last TX (heartbeat — proves
 *        the node is alive even when all readings are stable).
 *
 * Thresholds (all overridable via build flags):
 *   RIVR_SENSOR_TX_MS         heartbeat interval  (default 5 min)
 *   RIVR_SENSOR_MIN_DELTA_MS  min gap between delta-triggered TXes (30 s)
 *   RIVR_SENSOR_DELTA_TEMP    temp change to trigger in °C×100   (default 50 = 0.5°C)
 *   RIVR_SENSOR_DELTA_RH      RH change to trigger in %×100      (default 100 = 1%)
 *   RIVR_SENSOR_DELTA_VBAT    VBAT change to trigger in mV        (default 100 mV)
 *
 * Transmit schedule:
 *   T = 0 s  Trigger fires:
 *             • AM2302 read (blocking ~5 ms) — appended immediately.
 *             • VBAT ADC read (instant)      — appended immediately.
 *             • DS18B20 conversion started   — non-blocking, result in ~800 ms.
 *   T ≈ 0.8 s DS18B20 ready → append DS18B20 reading → send combined packet.
 *
 *   If DS18B20 is disabled the packet is sent at the trigger tick itself.
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
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdbool.h>

#define SENSORS_NVS_NS         "rivr_sens"
#define SENSORS_NVS_KEY_TX_MS  "tx_ms"
#define SENSORS_NVS_KEY_DLT_MS "dlt_ms"
#define SENSORS_NVS_KEY_D_TEMP "d_temp"
#define SENSORS_NVS_KEY_D_RH   "d_rh"
#define SENSORS_NVS_KEY_D_VBAT "d_vbat"

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

/* ── Transmit policy tunables ───────────────────────────────────────────── */

/** Heartbeat interval: send unconditionally even if nothing changed. */
#ifndef RIVR_SENSOR_TX_MS
#  define RIVR_SENSOR_TX_MS        300000u  /* 5 minutes */
#endif

/** Minimum gap between delta-triggered transmissions (burst suppression). */
#ifndef RIVR_SENSOR_MIN_DELTA_MS
#  define RIVR_SENSOR_MIN_DELTA_MS  30000u  /* 30 seconds */
#endif

/** Temperature delta that triggers an early TX (°C × 100). */
#ifndef RIVR_SENSOR_DELTA_TEMP
#  define RIVR_SENSOR_DELTA_TEMP      50   /* 0.50 °C */
#endif

/** Relative humidity delta that triggers an early TX (% × 100). */
#ifndef RIVR_SENSOR_DELTA_RH
#  define RIVR_SENSOR_DELTA_RH       100   /* 1.00 % */
#endif

/** Battery voltage delta that triggers an early TX (mV). */
#ifndef RIVR_SENSOR_DELTA_VBAT
#  define RIVR_SENSOR_DELTA_VBAT     100   /* 100 mV */
#endif

/** Runtime sensor config — updated by sensors_set_config() / sensors_nvs_load(). */
static sensors_config_t s_cfg = {
    .tx_ms        = RIVR_SENSOR_TX_MS,
    .min_delta_ms = RIVR_SENSOR_MIN_DELTA_MS,
    .delta_temp   = RIVR_SENSOR_DELTA_TEMP,
    .delta_rh     = RIVR_SENSOR_DELTA_RH,
    .delta_vbat   = RIVR_SENSOR_DELTA_VBAT,
};

#if RIVR_FEATURE_DS18B20 || RIVR_FEATURE_AM2302 || RIVR_FEATURE_VBAT
/** Monotonic timestamp of last sensor TX trigger (in ms). */
static uint32_t s_last_trigger_ms = 0u;

/** Set after first sensors_tick() call so we can time the first trigger. */
static bool s_started = false;

/* ── Last-sent values for delta detection ───────────────────────────────── */
static int32_t s_last_ds18b20  = INT32_MIN;
static int32_t s_last_am2302_rh   = INT32_MIN;
static int32_t s_last_am2302_temp = INT32_MIN;
static int32_t s_last_vbat        = INT32_MIN;

/* Helper: absolute difference clamped to positive. */
static inline int32_t abs_diff(int32_t a, int32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

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
             "sensors_init OK (DS18B20=%d AM2302=%d VBAT=%d tx_ms=%lu delta_temp=%u delta_rh=%u)",
             RIVR_FEATURE_DS18B20, RIVR_FEATURE_AM2302, RIVR_FEATURE_VBAT,
             (unsigned long)s_cfg.tx_ms, (unsigned)s_cfg.delta_temp, (unsigned)s_cfg.delta_rh);
#endif
}

/* ── Sensor config public API ──────────────────────────────────────────── */

void sensors_nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SENSORS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* no persisted config — keep compile-time defaults */
    }
    uint32_t u32;
    uint16_t u16;
    if (nvs_get_u32(h, SENSORS_NVS_KEY_TX_MS,  &u32) == ESP_OK && u32 >= 5000u)
        s_cfg.tx_ms = u32;
    if (nvs_get_u32(h, SENSORS_NVS_KEY_DLT_MS, &u32) == ESP_OK && u32 >= 1000u)
        s_cfg.min_delta_ms = u32;
    if (nvs_get_u16(h, SENSORS_NVS_KEY_D_TEMP, &u16) == ESP_OK)
        s_cfg.delta_temp = u16;
    if (nvs_get_u16(h, SENSORS_NVS_KEY_D_RH,   &u16) == ESP_OK)
        s_cfg.delta_rh = u16;
    if (nvs_get_u16(h, SENSORS_NVS_KEY_D_VBAT, &u16) == ESP_OK)
        s_cfg.delta_vbat = u16;
    nvs_close(h);
    ESP_LOGI(TAG, "sensor config loaded from NVS (tx_ms=%lu delta_temp=%u delta_rh=%u)",
             (unsigned long)s_cfg.tx_ms, (unsigned)s_cfg.delta_temp, (unsigned)s_cfg.delta_rh);
}

bool sensors_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(SENSORS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = true;
    if (nvs_set_u32(h, SENSORS_NVS_KEY_TX_MS,  s_cfg.tx_ms)        != ESP_OK) ok = false;
    if (nvs_set_u32(h, SENSORS_NVS_KEY_DLT_MS, s_cfg.min_delta_ms) != ESP_OK) ok = false;
    if (nvs_set_u16(h, SENSORS_NVS_KEY_D_TEMP, s_cfg.delta_temp)   != ESP_OK) ok = false;
    if (nvs_set_u16(h, SENSORS_NVS_KEY_D_RH,   s_cfg.delta_rh)     != ESP_OK) ok = false;
    if (nvs_set_u16(h, SENSORS_NVS_KEY_D_VBAT, s_cfg.delta_vbat)   != ESP_OK) ok = false;
    if (ok) { nvs_commit(h); }
    nvs_close(h);
    return ok;
}

sensors_config_t sensors_get_config(void)
{
    return s_cfg;
}

void sensors_set_config(const sensors_config_t *cfg)
{
    if (!cfg) return;
    /* Clamp to sane minimum values to prevent accidental duty-cycle violations. */
    s_cfg.tx_ms        = (cfg->tx_ms >= 5000u)        ? cfg->tx_ms        : 5000u;
    s_cfg.min_delta_ms = (cfg->min_delta_ms >= 1000u) ? cfg->min_delta_ms : 1000u;
    s_cfg.delta_temp   = cfg->delta_temp;
    s_cfg.delta_rh     = cfg->delta_rh;
    s_cfg.delta_vbat   = cfg->delta_vbat;
    ESP_LOGI(TAG, "sensor config updated (tx_ms=%lu min_dlt=%lu d_temp=%u d_rh=%u d_vbat=%u)",
             (unsigned long)s_cfg.tx_ms, (unsigned long)s_cfg.min_delta_ms,
             (unsigned)s_cfg.delta_temp, (unsigned)s_cfg.delta_rh, (unsigned)s_cfg.delta_vbat);
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

    /* ── Sample sensors for delta check (cheap reads only) ─────────────── */
    bool do_trigger = false;
    uint32_t elapsed = now_ms - s_last_trigger_ms;

    /* Heartbeat: always send after s_cfg.tx_ms. */
    if (elapsed >= s_cfg.tx_ms) {
        do_trigger = true;
        ESP_LOGI(TAG, "heartbeat trigger (%lu ms)", (unsigned long)elapsed);
    }

    /* Delta trigger: only when minimum silence has passed. */
    if (!do_trigger && elapsed >= s_cfg.min_delta_ms) {
#if RIVR_FEATURE_AM2302
        if (am2302_tick(&s_am_ctx, now_ms)) {
            int32_t rh   = am2302_get_rh_x100(&s_am_ctx);
            int32_t temp = am2302_get_temp_x100(&s_am_ctx);
            if (s_last_am2302_rh   != INT32_MIN &&
                abs_diff(rh,   s_last_am2302_rh)   >= s_cfg.delta_rh) {
                ESP_LOGI(TAG, "AM2302 RH delta trigger (%ld -> %ld x100 %%)",
                         (long)s_last_am2302_rh, (long)rh);
                do_trigger = true;
            }
            if (s_last_am2302_temp != INT32_MIN &&
                abs_diff(temp, s_last_am2302_temp) >= s_cfg.delta_temp) {
                ESP_LOGI(TAG, "AM2302 temp delta trigger (%ld -> %ld x100 degC)",
                         (long)s_last_am2302_temp, (long)temp);
                do_trigger = true;
            }
            /* Stash latest sample — will be re-read properly in the trigger block. */
            (void)rh; (void)temp;
        }
#endif
#if RIVR_FEATURE_VBAT
        if (!do_trigger) {
            int32_t vbat_mv = vbat_read_mv();
            if (s_last_vbat != INT32_MIN &&
                abs_diff(vbat_mv, s_last_vbat) >= s_cfg.delta_vbat) {
                ESP_LOGI(TAG, "VBAT delta trigger (%ld -> %ld mV)",
                         (long)s_last_vbat, (long)vbat_mv);
                do_trigger = true;
            }
        }
#endif
    }

    /* ── Trigger: build and (possibly) send bundle ───────────────────────── */
    if (do_trigger) {
        s_last_trigger_ms = now_ms;

        /* Reset bundle and record context for send_bundle() */
        s_bundle_count   = 0u;
        s_bundle_ts_s    = now_ms / 1000u;
        s_bundle_node_id = node_id;
        s_bundle_net_id  = net_id;
        s_ds18b20_pending = false;

#if RIVR_FEATURE_AM2302
        /* am2302_tick() blocks ~5 ms — acceptable at a ≥ 30 s interval. */
        if (am2302_tick(&s_am_ctx, now_ms)) {
            int32_t rh   = am2302_get_rh_x100(&s_am_ctx);
            int32_t temp = am2302_get_temp_x100(&s_am_ctx);
            bundle_append(SENSOR_ID_AM2302_RH,   rh,   UNIT_PERCENT_RH);
            bundle_append(SENSOR_ID_AM2302_TEMP, temp, UNIT_CELSIUS);
            s_last_am2302_rh   = rh;
            s_last_am2302_temp = temp;
        }
#endif

#if RIVR_FEATURE_VBAT
        {
            int32_t vbat_mv = vbat_read_mv();
            if (vbat_mv > 0) {
                bundle_append(SENSOR_ID_VBAT, vbat_mv, UNIT_MILLIVOLTS);
                s_last_vbat = vbat_mv;
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
    } /* end do_trigger */

    /* ── DS18B20 tick: advance conversion state machine ─────────────────── */
#if RIVR_FEATURE_DS18B20
    ds18b20_tick(&s_ds_ctx, now_ms);

    if (s_ds18b20_pending) {
        if (ds18b20_ready(&s_ds_ctx)) {
            int32_t temp = ds18b20_read_celsius_x100(&s_ds_ctx);
            bundle_append(SENSOR_ID_DS18B20_TEMP, temp, UNIT_CELSIUS);
            s_last_ds18b20 = temp;
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

