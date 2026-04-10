/**
 * @file  rivr_ble_service.c
 * @brief Rivr BLE GATT service: RX write handler + TX notify (Bluedroid).
 */

#include "rivr_ble_service.h"
#include "rivr_ble.h"
#include "rivr_ble_companion.h"
#include "rivr_ble_frag.h"
#include "rivr_ble_service_internal.h"
#include "rivr_config.h"

#if RIVR_FEATURE_BLE

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_gatt_common_api.h"

#include "../radio_sx1262.h"
#include "../rivr_log.h"
#include "../rivr_metrics.h"
#include "../timebase.h"
#include "../ringbuf.h"

#define TAG "RIVR_BLE_SVC"
#define RIVR_BLE_APP_ID 0x56

enum {
    IDX_SVC,
    IDX_RX_CHAR_DECL,
    IDX_RX_CHAR_VAL,
    IDX_TX_CHAR_DECL,
    IDX_TX_CHAR_VAL,
    IDX_TX_CCCD,
    IDX_NB,
};

static const uint16_t s_primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_char_decl_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_cccd_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t s_svc_uuid[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e,
};
static const uint8_t s_rx_uuid[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e,
};
static const uint8_t s_tx_uuid[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e,
};

static const uint8_t s_rx_props =
    ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t s_tx_props = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t s_cccd_default[2] = {0x00, 0x00};
static const uint8_t s_dummy_val[1] = {0x00};

#if RIVR_BLE_PASSKEY != 0
#define RIVR_BLE_RX_PERM   ESP_GATT_PERM_WRITE_ENC_MITM
#define RIVR_BLE_TX_PERM   ESP_GATT_PERM_READ_ENC_MITM
#define RIVR_BLE_CCCD_PERM (ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM)
#else
#define RIVR_BLE_RX_PERM   ESP_GATT_PERM_WRITE
#define RIVR_BLE_TX_PERM   ESP_GATT_PERM_READ
#define RIVR_BLE_CCCD_PERM (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE)
#endif

static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t *)&s_primary_service_uuid,
            ESP_GATT_PERM_READ,
            sizeof(s_svc_uuid), sizeof(s_svc_uuid), (uint8_t *)s_svc_uuid,
        },
    },
    [IDX_RX_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t *)&s_char_decl_uuid,
            ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_rx_props,
        },
    },
    [IDX_RX_CHAR_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_128, (uint8_t *)s_rx_uuid,
            RIVR_BLE_RX_PERM,
            RF_MAX_PAYLOAD_LEN, sizeof(s_dummy_val), (uint8_t *)s_dummy_val,
        },
    },
    [IDX_TX_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t *)&s_char_decl_uuid,
            ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_tx_props,
        },
    },
    [IDX_TX_CHAR_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_128, (uint8_t *)s_tx_uuid,
            RIVR_BLE_TX_PERM,
            RF_MAX_PAYLOAD_LEN, sizeof(s_dummy_val), (uint8_t *)s_dummy_val,
        },
    },
    [IDX_TX_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t *)&s_cccd_uuid,
            RIVR_BLE_CCCD_PERM,
            sizeof(uint16_t), sizeof(s_cccd_default), (uint8_t *)s_cccd_default,
        },
    },
};

static uint16_t s_handle_table[IDX_NB];
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFFu;
static bool s_service_ready = false;
static bool s_notify_enabled = false;
static rivr_ble_frag_rx_t s_rx_frag;
static uint8_t s_tx_frag_msg_id = 0u;

static bool rivr_ble_service_notify_one(void *ctx, const uint8_t *data, uint16_t len)
{
    const uint16_t *conn_handle = (const uint16_t *)ctx;
    esp_err_t err;
    uint8_t attempt;

    (void)conn_handle;

    for (attempt = 0u; attempt < 3u; ++attempt) {
        err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                          s_handle_table[IDX_TX_CHAR_VAL],
                                          (uint16_t)len, (uint8_t *)data, false);
        if (err == ESP_OK) {
            return true;
        }
        if (attempt + 1u < 3u) {
            vTaskDelay(pdMS_TO_TICKS(12));
        }
    }

    RIVR_LOGD(TAG, "BLE notify failed: %s", esp_err_to_name(err));
    return false;
}

static bool rivr_ble_service_handle_payload(const uint8_t *data, uint16_t len)
{
    rf_rx_frame_t frame;

    if (!data || len == 0u || len > RF_MAX_PAYLOAD_LEN) {
        RIVR_LOGW(TAG, "BLE rx: frame length %u out of range (max=%u)",
                  (unsigned)len, (unsigned)RF_MAX_PAYLOAD_LEN);
        g_rivr_metrics.ble_errors++;
        return false;
    }

    if (rivr_ble_companion_handle_rx(data, len)) {
        return true;
    }

    memset(&frame, 0, sizeof(frame));
    memcpy(frame.data, data, len);
    frame.len = (uint8_t)len;
    frame.rssi_dbm = 0;
    frame.snr_db = 0;
    frame.rx_mono_ms = tb_millis();
    frame.from_id = 0u;
    frame.iface = 1u;

    if (!rb_try_push(&rf_rx_ringbuf, &frame)) {
        RIVR_LOGW(TAG, "BLE rx: rf_rx_ringbuf full — frame dropped");
        g_rivr_metrics.ble_errors++;
        return false;
    }

    g_rivr_metrics.ble_rx_frames++;
    RIVR_LOGI(TAG, "BLE rx: %u bytes pushed to rf_rx_ringbuf", (unsigned)len);
    return true;
}

static void rivr_ble_service_handle_rx_write(const esp_ble_gatts_cb_param_t *param)
{
    const struct gatts_write_evt_param *w = &param->write;
    const uint8_t *payload = NULL;
    uint16_t payload_len = 0u;
    rivr_ble_frag_rx_result_t frag_result;

    frag_result = rivr_ble_frag_ingest(&s_rx_frag, w->value, w->len,
                                       &payload, &payload_len);
    if (frag_result == RIVR_BLE_FRAG_RX_INVALID) {
        RIVR_LOGW(TAG, "BLE rx: invalid fragment stream");
        g_rivr_metrics.ble_errors++;
        return;
    }
    if (frag_result == RIVR_BLE_FRAG_RX_INCOMPLETE) {
        return;
    }

    (void)rivr_ble_service_handle_payload(payload, payload_len);
}

int rivr_ble_service_register(void)
{
    esp_err_t err = esp_ble_gatts_app_register(RIVR_BLE_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gatts_app_register failed: %s", esp_err_to_name(err));
    }
    return (int)err;
}

void rivr_ble_service_set_connection(esp_gatt_if_t gatts_if, uint16_t conn_id)
{
    s_gatts_if = gatts_if;
    s_conn_id = conn_id;
}

void rivr_ble_service_clear_connection(void)
{
    s_conn_id = 0xFFFFu;
    s_notify_enabled = false;
    rivr_ble_frag_reset(&s_rx_frag);
}

bool rivr_ble_service_is_ready(void)
{
    return s_service_ready;
}

void rivr_ble_service_handle_gatts_event(esp_gatts_cb_event_t event,
                                         esp_gatt_if_t gatts_if,
                                         esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTS reg failed: %d", param->reg.status);
            return;
        }
        s_gatts_if = gatts_if;
        esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "attr table create failed: %d", param->add_attr_tab.status);
            return;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "attr table handle count mismatch: got=%u expected=%u",
                     (unsigned)param->add_attr_tab.num_handle, (unsigned)IDX_NB);
            return;
        }
        memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
        esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
        s_service_ready = true;
        RIVR_LOGI(TAG, "GATT service ready");
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_handle_table[IDX_RX_CHAR_VAL]) {
            rivr_ble_service_handle_rx_write(param);
        } else if (param->write.handle == s_handle_table[IDX_TX_CCCD] &&
                   param->write.len >= 2u) {
            uint16_t cccd = (uint16_t)param->write.value[0] |
                            ((uint16_t)param->write.value[1] << 8);
            s_notify_enabled = (cccd & 0x0001u) != 0u;
            RIVR_LOGI(TAG, "BLE notify %s", s_notify_enabled ? "enabled" : "disabled");
        }
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        rivr_ble_companion_on_disconnect();
        rivr_ble_service_clear_connection();
        break;

    default:
        break;
    }
}

bool rivr_ble_service_notify(uint16_t conn_handle, const uint8_t *data, uint8_t len)
{
    uint16_t payload_limit = rivr_ble_link_payload_limit();

    if (s_gatts_if == ESP_GATT_IF_NONE || s_conn_id == 0xFFFFu) return false;
    if (!s_service_ready || !s_notify_enabled || !data || len == 0u) return false;
    if (payload_limit == 0u) {
        g_rivr_metrics.ble_errors++;
        RIVR_LOGW(TAG, "BLE notify skipped: len=%u payload_limit=%u",
                  (unsigned)len, (unsigned)payload_limit);
        return false;
    }
    if (rivr_ble_frag_send(data, len, payload_limit, &s_tx_frag_msg_id,
                           rivr_ble_service_notify_one, &conn_handle)) {
        g_rivr_metrics.ble_tx_frames++;
        return true;
    }

    g_rivr_metrics.ble_errors++;
    return false;
}

#endif /* RIVR_FEATURE_BLE */
