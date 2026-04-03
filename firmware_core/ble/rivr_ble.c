/**
 * @file  rivr_ble.c
 * @brief Rivr BLE transport — Bluedroid stack init, advertising, connection lifecycle.
 */

#include "rivr_ble.h"
#include "rivr_config.h"

#if RIVR_FEATURE_BLE

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"

#include "rivr_ble_service.h"
#include "rivr_ble_service_internal.h"
#include "../rivr_log.h"
#include "../rivr_metrics.h"

extern uint32_t g_my_node_id;

#define TAG "RIVR_BLE"
#define ADV_CONFIG_FLAG      (1u << 0)
#define SCAN_RSP_CONFIG_FLAG (1u << 1)

#if RIVR_BLE_PASSKEY != 0
#define RIVR_BLE_DEFAULT_SENTINEL_PASSKEY 123456UL
#endif

static volatile uint16_t s_conn_handle = 0xFFFFu;
static volatile uint16_t s_pending_conn_handle = 0xFFFFu;
#if RIVR_BLE_PASSKEY != 0
static uint32_t s_active_passkey = 0u;
#endif
static volatile bool s_ble_active = false;
static volatile bool s_stack_ready = false;
static volatile bool s_adv_running = false;
static uint32_t s_activate_ms = 0u;
static uint32_t s_timeout_ms = BLE_BOOT_WINDOW_MS;
static uint8_t s_adv_config_done = 0u;
static esp_bd_addr_t s_remote_bda = {0};
static bool s_remote_bda_valid = false;
static char s_device_name[16];

static const uint8_t s_adv_svc_uuid[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = false,
    .service_uuid_len = sizeof(s_adv_svc_uuid),
    .p_service_uuid = (uint8_t *)s_adv_svc_uuid,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x100,
    .adv_int_max = 0x100,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#if RIVR_BLE_PASSKEY != 0
static uint32_t rivr_ble_choose_passkey(void)
{
#if RIVR_BLE_RANDOM_PASSKEY
    (void)RIVR_BLE_DEFAULT_SENTINEL_PASSKEY;
    return 100000u + (esp_random() % 900000u);
#endif
    return (uint32_t)RIVR_BLE_PASSKEY;
}

static bool rivr_ble_auth_mode_is_mitm(esp_ble_auth_req_t auth_mode)
{
    switch (auth_mode) {
    case ESP_LE_AUTH_REQ_MITM:
    case ESP_LE_AUTH_REQ_BOND_MITM:
    case ESP_LE_AUTH_REQ_SC_MITM:
    case ESP_LE_AUTH_REQ_SC_MITM_BOND:
        return true;
    default:
        return false;
    }
}
#endif

static void rivr_ble_set_device_name(void)
{
    snprintf(s_device_name, sizeof(s_device_name), "RIVR-%04X",
             (unsigned)(g_my_node_id & 0xFFFFu));
    esp_ble_gap_set_device_name(s_device_name);
    RIVR_LOGI(TAG, "BLE device name: %s", s_device_name);
}

static void rivr_ble_start_adv(void)
{
    if (!s_ble_active || !s_stack_ready || !rivr_ble_service_is_ready()) return;
    if (s_conn_handle != 0xFFFFu || s_pending_conn_handle != 0xFFFFu) return;
    if (s_adv_config_done != 0u) return;

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    if (err != ESP_OK) {
        RIVR_LOGW(TAG, "esp_ble_gap_start_advertising failed: %s", esp_err_to_name(err));
    }
}

static void rivr_ble_gap_event(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= (uint8_t)~ADV_CONFIG_FLAG;
        rivr_ble_start_adv();
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= (uint8_t)~SCAN_RSP_CONFIG_FLAG;
        rivr_ble_start_adv();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_adv_running = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        if (!s_adv_running) {
            RIVR_LOGW(TAG, "BLE advertising start failed (status=%d)",
                      param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_adv_running = false;
        break;

#if RIVR_BLE_PASSKEY != 0
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        RIVR_LOGI(TAG, "BLE pairing: show passkey %06" PRIu32,
                  param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        RIVR_LOGI(TAG, "BLE pairing: peer requested passkey entry");
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
        RIVR_LOGW(TAG, "BLE pairing: unexpected numeric comparison %06" PRIu32 " auto-accepted",
                  param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success &&
            rivr_ble_auth_mode_is_mitm(param->ble_security.auth_cmpl.auth_mode)) {
            s_conn_handle = s_pending_conn_handle;
            RIVR_LOGI(TAG, "BLE encrypted & authenticated (conn_handle=0x%04x) — ready",
                      (unsigned)s_conn_handle);
        } else {
            RIVR_LOGW(TAG, "BLE pairing failed (success=%d auth_mode=%u reason=0x%x)",
                      param->ble_security.auth_cmpl.success,
                      (unsigned)param->ble_security.auth_cmpl.auth_mode,
                      (unsigned)param->ble_security.auth_cmpl.fail_reason);
            if (s_remote_bda_valid) {
                esp_ble_gap_disconnect(s_remote_bda);
            }
        }
        s_pending_conn_handle = 0xFFFFu;
        break;
#endif

    default:
        break;
    }
}

#if RIVR_BLE_PASSKEY != 0
static bool rivr_ble_is_peer_bonded(const esp_bd_addr_t bda);
#endif

static void rivr_ble_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    rivr_ble_service_handle_gatts_event(event, gatts_if, param);

    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            RIVR_LOGW(TAG, "GATTS reg failed: %d", param->reg.status);
            return;
        }
        rivr_ble_set_device_name();
        s_adv_config_done = ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG;
        esp_ble_gap_config_adv_data(&s_adv_data);
        esp_ble_gap_config_adv_data(&s_scan_rsp_data);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        rivr_ble_start_adv();
        break;

    case ESP_GATTS_CONNECT_EVT:
        g_rivr_metrics.ble_connections++;
        s_adv_running = false;
        memcpy(s_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        s_remote_bda_valid = true;
        rivr_ble_service_set_connection(gatts_if, param->connect.conn_id);
#if RIVR_BLE_PASSKEY != 0
        s_pending_conn_handle = param->connect.conn_id;
        if (rivr_ble_is_peer_bonded(param->connect.remote_bda)) {
            /* Already bonded — reuse stored LTK; no new passkey challenge needed. */
            RIVR_LOGI(TAG, "BLE reconnect from bonded peer (conn_id=0x%04x) — re-encrypting",
                      (unsigned)s_pending_conn_handle);
            esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
        } else {
            /* New peer — full MITM passkey exchange. */
            RIVR_LOGI(TAG, "BLE new peer connected (conn_id=0x%04x) — requesting MITM encryption",
                      (unsigned)s_pending_conn_handle);
            esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
        }
#else
        s_conn_handle = param->connect.conn_id;
        RIVR_LOGI(TAG, "BLE connected (conn_id=0x%04x)", (unsigned)s_conn_handle);
#endif
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        RIVR_LOGI(TAG, "BLE disconnected (reason=0x%x)", param->disconnect.reason);
        s_conn_handle = 0xFFFFu;
#if RIVR_BLE_PASSKEY != 0
        s_pending_conn_handle = 0xFFFFu;
#endif
        s_remote_bda_valid = false;
        rivr_ble_service_clear_connection();
        if (s_ble_active) {
            rivr_ble_start_adv();
        }
        break;

    default:
        break;
    }
}

void rivr_ble_init(void)
{
    esp_err_t err;

    RIVR_LOGI(TAG, "rivr_ble_init: initialising Bluedroid BLE stack");

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_mem_release failed: %s", esp_err_to_name(err));
        return;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ble_gatts_register_callback(rivr_ble_gatts_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gatts_register_callback failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ble_gap_register_callback(rivr_ble_gap_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_register_callback failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ble_gatt_set_local_mtu(247);
    if (err != ESP_OK) {
        RIVR_LOGW(TAG, "esp_ble_gatt_set_local_mtu failed: %s", esp_err_to_name(err));
    }

#if RIVR_BLE_PASSKEY != 0
    s_active_passkey = rivr_ble_choose_passkey();
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY,
                                   &s_active_passkey, sizeof(s_active_passkey));
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
                                   &auth_option, sizeof(auth_option));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(oob_support));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    RIVR_LOGI(TAG, "BLE security: Bluedroid MITM passkey bonding enabled (PIN=%06lu)",
              (unsigned long)s_active_passkey);
#else
    RIVR_LOGI(TAG, "BLE security: open (no pairing)");
#endif

    err = rivr_ble_service_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rivr_ble_service_register failed: %s", esp_err_to_name((esp_err_t)err));
        return;
    }

    s_stack_ready = true;
    s_ble_active = true;
    s_activate_ms = 0u;
#if RIVR_BLE_PASSKEY != 0
    s_timeout_ms = 0u;
#else
    s_timeout_ms = BLE_BOOT_WINDOW_MS;
#endif
    s_conn_handle = 0xFFFFu;
#if RIVR_BLE_PASSKEY != 0
    s_pending_conn_handle = 0xFFFFu;
#endif

    if (s_timeout_ms == 0u) {
        RIVR_LOGI(TAG, "Bluedroid BLE active without timeout");
    } else {
        RIVR_LOGI(TAG, "Bluedroid BLE active (%lu ms window)",
                  (unsigned long)BLE_BOOT_WINDOW_MS);
    }
}

void rivr_ble_tick(uint32_t now_ms)
{
    if (s_activate_ms == 0u && s_ble_active) {
        s_activate_ms = now_ms;
    }
    if (!s_ble_active) return;
    if (s_timeout_ms == 0u) return;
    if ((now_ms - s_activate_ms) >= s_timeout_ms) {
        RIVR_LOGI(TAG, "BLE: activation window expired (%lu ms) — deactivating",
                  (unsigned long)s_timeout_ms);
        rivr_ble_deactivate();
    }
}

void rivr_ble_activate(rivr_ble_mode_t mode)
{
    switch (mode) {
    case RIVR_BLE_MODE_BOOT_WINDOW:
#if RIVR_BLE_PASSKEY != 0
        s_timeout_ms = 0u;
#else
        s_timeout_ms = BLE_BOOT_WINDOW_MS;
#endif
        break;
    case RIVR_BLE_MODE_BUTTON:
        s_timeout_ms = BLE_BUTTON_WINDOW_MS;
        break;
    case RIVR_BLE_MODE_APP_REQUESTED:
        s_timeout_ms = 0u;
        break;
    default:
        s_timeout_ms = BLE_BOOT_WINDOW_MS;
        break;
    }

    s_ble_active = true;
    s_activate_ms = 0u;
    rivr_ble_start_adv();
}

void rivr_ble_deactivate(void)
{
    s_ble_active = false;
    s_timeout_ms = 0u;

    if (s_adv_running) {
        esp_ble_gap_stop_advertising();
    }
    if (s_remote_bda_valid) {
        esp_ble_gap_disconnect(s_remote_bda);
    }
}

bool rivr_ble_is_active(void)
{
    return s_ble_active;
}

bool rivr_ble_is_connected(void)
{
    return s_conn_handle != 0xFFFFu;
}

uint16_t rivr_ble_conn_handle(void)
{
    return s_conn_handle;
}

uint32_t rivr_ble_passkey(void)
{
#if RIVR_BLE_PASSKEY != 0
    return s_active_passkey;
#else
    return 0u;
#endif
}

bool rivr_ble_has_bond(void)
{
#if RIVR_BLE_PASSKEY != 0
    return esp_ble_get_bond_device_num() > 0;
#else
    return false;
#endif
}

#if RIVR_BLE_PASSKEY != 0
/**
 * Returns true if @p bda is already in the Bluedroid bond store.
 * Used on reconnect to skip MITM re-challenge for a previously paired peer.
 */
static bool rivr_ble_is_peer_bonded(const esp_bd_addr_t bda)
{
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num <= 0) return false;

    esp_ble_bond_dev_t *list = calloc((size_t)dev_num, sizeof(*list));
    if (!list) return false;

    int count = dev_num;
    bool found = false;
    if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            if (memcmp(list[i].bd_addr, bda, sizeof(esp_bd_addr_t)) == 0) {
                found = true;
                break;
            }
        }
    }
    free(list);
    return found;
}
#endif

int rivr_ble_clear_bonds(void)
{
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num <= 0) {
        if (s_remote_bda_valid) {
            esp_ble_gap_disconnect(s_remote_bda);
        }
        return 0;
    }

    esp_ble_bond_dev_t *dev_list = calloc((size_t)dev_num, sizeof(*dev_list));
    if (dev_list == NULL) {
        RIVR_LOGW(TAG, "BLE clear bonds: allocation failed for %d entries", dev_num);
        return -1;
    }

    int list_count = dev_num;
    esp_err_t err = esp_ble_get_bond_device_list(&list_count, dev_list);
    if (err != ESP_OK) {
        RIVR_LOGW(TAG, "BLE clear bonds: list fetch failed: %s", esp_err_to_name(err));
        free(dev_list);
        return -1;
    }

    if (s_remote_bda_valid) {
        esp_ble_gap_disconnect(s_remote_bda);
        s_conn_handle = 0xFFFFu;
        s_pending_conn_handle = 0xFFFFu;
        s_remote_bda_valid = false;
        rivr_ble_service_clear_connection();
    }

    int removed = 0;
    for (int i = 0; i < list_count; ++i) {
        err = esp_ble_remove_bond_device(dev_list[i].bd_addr);
        if (err == ESP_OK) {
            removed++;
        } else {
            RIVR_LOGW(TAG, "BLE clear bonds: remove failed for entry %d: %s",
                      i, esp_err_to_name(err));
        }
    }

    free(dev_list);

    RIVR_LOGI(TAG, "BLE clear bonds: removed %d/%d records", removed, list_count);
    if (s_ble_active) {
        rivr_ble_start_adv();
    }
    return removed;
}

#endif /* RIVR_FEATURE_BLE */
