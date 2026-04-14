/**
 * @file  sensors.c
 * @brief DS18B20 + AM2302 sensor subsystem — periodic PKT_TELEMETRY origination.
 *
 * Transmit schedule:
 *   • Every RIVR_SENSOR_TX_MS (default 60 s) this module:
 *       1. Triggers a DS18B20 temperature conversion (non-blocking, ~800 ms).
 *       2. Issues an AM2302 read (brief blocking, ~5 ms).
 *   • DS18B20 result is sent immediately after the conversion finishes
 *     (~800 ms after trigger), checked every loop tick.
 *   • AM2302 result (RH + temp, two frames) is sent at the same 60 s tick.
 *
 * Wire format: PKT_TELEMETRY, SVC_TELEMETRY_PAYLOAD_LEN = 11 bytes
 *   [0-1]  sensor_id  u16 LE   (1 = DS18B20 temp; 2 = AM2302 RH; 3 = AM2302 temp)
 *   [2-5]  value      i32 LE   (scaled × 100)
 *   [6]    unit_code  u8       (UNIT_CELSIUS or UNIT_PERCENT_RH)
 *   [7-10] timestamp  u32 LE   (seconds since boot, from now_ms / 1000)
 */

#include "sensors.h"
#include "sensor_ds18b20.h"
#include "sensor_am2302.h"
#include "protocol.h"
#include "iface/rivr_iface_lora.h"
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

/* ── State ──────────────────────────────────────────────────────────────── */
#if RIVR_FEATURE_DS18B20
static ds18b20_ctx_t s_ds_ctx;
#endif

#if RIVR_FEATURE_AM2302
static am2302_ctx_t s_am_ctx;
#endif

/** Monotonic timestamp of last sensor TX trigger (in ms). */
static uint32_t s_last_trigger_ms = 0u;

/** Set after first sensors_tick() call so we can time the first trigger. */
static bool s_started = false;

/* ── Internal helpers ───────────────────────────────────────────────────── */

/**
 * Build and push one PKT_TELEMETRY frame to the LoRa TX queue.
 *
 * @param sensor_id   Application sensor identifier (1/2/3).
 * @param value       Scaled value (e.g. °C × 100).
 * @param unit_code   UNIT_CELSIUS / UNIT_PERCENT_RH / …
 * @param timestamp_s Seconds since boot (now_ms / 1000).
 * @param node_id     Source node ID.
 * @param net_id      Network ID.
 */
static void send_telemetry(uint16_t sensor_id, int32_t value, uint8_t unit_code,
                           uint32_t timestamp_s,
                           uint32_t node_id, uint16_t net_id)
{
    /* Build 11-byte telemetry payload (all fields little-endian) */
    uint8_t payload[SVC_TELEMETRY_PAYLOAD_LEN];
    payload[0]  = (uint8_t)(sensor_id & 0xFFu);
    payload[1]  = (uint8_t)((sensor_id >> 8) & 0xFFu);
    payload[2]  = (uint8_t)((uint32_t)value & 0xFFu);
    payload[3]  = (uint8_t)(((uint32_t)value >> 8) & 0xFFu);
    payload[4]  = (uint8_t)(((uint32_t)value >> 16) & 0xFFu);
    payload[5]  = (uint8_t)(((uint32_t)value >> 24) & 0xFFu);
    payload[6]  = unit_code;
    payload[7]  = (uint8_t)(timestamp_s & 0xFFu);
    payload[8]  = (uint8_t)((timestamp_s >> 8) & 0xFFu);
    payload[9]  = (uint8_t)((timestamp_s >> 16) & 0xFFu);
    payload[10] = (uint8_t)((timestamp_s >> 24) & 0xFFu);

    /* Build RIVR packet header */
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_TELEMETRY;
    hdr.flags       = 0u;
    hdr.ttl         = RIVR_PKT_DEFAULT_TTL;
    hdr.hop         = 0u;
    hdr.net_id      = net_id;
    hdr.src_id      = node_id;
    hdr.dst_id      = 0u;  /* broadcast */
    hdr.seq         = (uint16_t)++g_ctrl_seq;
    hdr.pkt_id      = (uint16_t)g_ctrl_seq;
    hdr.payload_len = SVC_TELEMETRY_PAYLOAD_LEN;

    /* Encode into wire format */
    uint8_t buf[RIVR_PKT_HDR_LEN + SVC_TELEMETRY_PAYLOAD_LEN + 2u /* CRC */];
    int enc = protocol_encode(&hdr, payload, SVC_TELEMETRY_PAYLOAD_LEN,
                              buf, (uint8_t)sizeof(buf));
    if (enc <= 0) {
        ESP_LOGW(TAG, "protocol_encode failed for sensor_id=%u", (unsigned)sensor_id);
        return;
    }

    /* Push to LoRa TX queue */
    if (!rivr_iface_lora_send(buf, (size_t)enc)) {
        ESP_LOGW(TAG, "TX queue full — telemetry sensor_id=%u dropped",
                 (unsigned)sensor_id);
        return;
    }
    ESP_LOGI(TAG, "queued telemetry sensor=%u value=%ld unit=%u ts=%lu",
             (unsigned)sensor_id, (long)value, (unsigned)unit_code,
             (unsigned long)timestamp_s);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void sensors_init(void)
{
#if RIVR_FEATURE_DS18B20
    ds18b20_init(&s_ds_ctx, PIN_DS18B20_ONEWIRE);
#endif
#if RIVR_FEATURE_AM2302
    am2302_init(&s_am_ctx, PIN_AM2302_DATA);
#endif
#if RIVR_FEATURE_DS18B20 || RIVR_FEATURE_AM2302
    ESP_LOGI(TAG, "sensors_init OK (DS18B20=%d AM2302=%d pin_ds=%d pin_am=%d)",
             RIVR_FEATURE_DS18B20, RIVR_FEATURE_AM2302,
             (int)PIN_DS18B20_ONEWIRE, (int)PIN_AM2302_DATA);
#endif
}

void sensors_tick(uint32_t now_ms, uint32_t node_id, uint16_t net_id)
{
#if !RIVR_FEATURE_DS18B20 && !RIVR_FEATURE_AM2302
    (void)now_ms; (void)node_id; (void)net_id;
    return;
#else
    uint32_t timestamp_s = now_ms / 1000u;

    /* Record time of first call so we can measure elapsed time consistently. */
    if (!s_started) {
        s_started = true;
        s_last_trigger_ms = now_ms;
    }

    /* ── Periodic trigger: start DS18B20 conversion + read AM2302 ──────── */
    if ((now_ms - s_last_trigger_ms) >= RIVR_SENSOR_TX_MS) {
        s_last_trigger_ms = now_ms;

#if RIVR_FEATURE_DS18B20
        ds18b20_start_conversion(&s_ds_ctx, now_ms);
#endif

#if RIVR_FEATURE_AM2302
        /* am2302_tick() blocks ~5 ms for the data read — acceptable here since
         * this branch is taken only once per RIVR_SENSOR_TX_MS (≥ 60 s).   */
        if (am2302_tick(&s_am_ctx, now_ms)) {
            send_telemetry(SENSOR_ID_AM2302_RH,
                           am2302_get_rh_x100(&s_am_ctx),
                           UNIT_PERCENT_RH,
                           timestamp_s, node_id, net_id);
            send_telemetry(SENSOR_ID_AM2302_TEMP,
                           am2302_get_temp_x100(&s_am_ctx),
                           UNIT_CELSIUS,
                           timestamp_s, node_id, net_id);
        }
#endif
    }

    /* ── DS18B20 tick: advance conversion state machine every loop tick ── */
#if RIVR_FEATURE_DS18B20
    ds18b20_tick(&s_ds_ctx, now_ms);

    if (ds18b20_ready(&s_ds_ctx)) {
        int32_t temp = ds18b20_read_celsius_x100(&s_ds_ctx);
        send_telemetry(SENSOR_ID_DS18B20_TEMP,
                       temp,
                       UNIT_CELSIUS,
                       timestamp_s, node_id, net_id);
    }
#endif

#endif /* at least one feature enabled */
}
