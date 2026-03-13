/**
 * @file  rivr_ble.c
 * @brief Rivr BLE transport — NimBLE stack init, advertising, connection lifecycle.
 *
 * INITIALISATION SEQUENCE
 * ────────────────────────
 *  rivr_ble_init()
 *    ├─ nimble_port_init()             — initialise NimBLE port layer
 *    ├─ ble_hs_cfg                     — set sync + reset callbacks
 *    ├─ ble_svc_gap_init()             — standard GAP service
 *    ├─ ble_svc_gatt_init()            — standard GATT service
 *    ├─ rivr_ble_service_register()    — Rivr UART service (RX/TX chars)
 *    └─ nimble_port_freertos_init()    — start NimBLE host FreeRTOS task
 *
 *  ble_hs_on_sync fires (from NimBLE host task, ~200 ms after init)
 *    └─ rivr_ble_start_adv()           — set device name, start advertising
 *
 * ADVERTISING
 * ────────────
 *  Name format: "RIVR-XXXX" where XXXX = last 2 bytes of g_my_node_id.
 *  Payload includes 128-bit Rivr service UUID for BLE scanner identification.
 *  Interval: 500–1000 ms (background scan budget; not performance-critical).
 *
 * TIMEOUT / ACTIVATION
 * ─────────────────────
 *  rivr_ble_tick(now_ms) is called from the main loop.  When the activation
 *  window expires, it calls ble_gap_adv_stop() and disconnects any client.
 *  This ensures BLE does not perpetually consume ~5 mA radio power.
 *
 * RECONNECT ON DISCONNECT
 * ────────────────────────
 *  The GAP event handler automatically restarts advertising on disconnect,
 *  as long as the activation window has not expired.  This lets the phone
 *  reconnect without rebooting the node.
 *
 * MEMORY
 * ──────
 *  NimBLE heap budget is controlled by sdkconfig.ble:
 *    CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 1
 *    CONFIG_BT_NIMBLE_MAX_BONDS       = 1
 *    CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU = 128
 *  Empirical footprint: ~40-55 KB BSS + NimBLE pool (on ESP32 with IDF 5.x).
 */

#include "rivr_ble.h"
#include "rivr_config.h"

#if RIVR_FEATURE_BLE

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* NimBLE headers */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Rivr firmware headers */
#include "rivr_ble_service.h"
#include "../rivr_metrics.h"
#include "../rivr_log.h"
/* g_my_node_id is defined in rivr_layer/rivr_embed.c.
 * Declare only the extern here to avoid pulling in all of rivr_embed.h. */
extern uint32_t g_my_node_id;

/* ── Forward declaration ─────────────────────────────────────────────────── */
/* rivr_ble_start_adv() references this callback before its definition. */
static int rivr_ble_gap_event(struct ble_gap_event *event, void *arg);

#define TAG "RIVR_BLE"

/* ── Module state ────────────────────────────────────────────────────────── *
 * s_conn_handle: written by NimBLE task in gap_event_cb,                   *
 *               read by main-loop task in is_connected()/service_notify(). *
 * volatile provides single-field torn-read protection on 32-bit Xtensa;    *
 * a full mutex is not necessary for this diagnostic/gate usage.            */

/** Current connection handle; BLE_HS_CONN_HANDLE_NONE = no client. */
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/** True while BLE stack is advertising or has an active connection. */
static volatile bool s_ble_active = false;

/** Monotonic ms at last rivr_ble_activate() call. */
static uint32_t s_activate_ms = 0u;

/** Auto-deactivation timeout in ms; 0 = no timeout (APP_REQUESTED mode). */
static uint32_t s_timeout_ms  = BLE_BOOT_WINDOW_MS;

/** Prevents rivr_start_adv() from running before the host is synced. */
static volatile bool s_host_synced = false;

/* ── Service UUID (used in advertising payload) ───────────────────────────── *
 * 6E400001-B5A3-F393-E0A9-E50E24DCCA9E in little-endian byte order.         */
static const ble_uuid128_t s_adv_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

/* ── Advertising helpers ─────────────────────────────────────────────────── */

/**
 * @brief Build advertising name from g_my_node_id and set the GAP device name.
 *
 * Result: "RIVR-XXXX" where XXXX = lower 16 bits of g_my_node_id as hex.
 * Example: node_id = 0xAB12CD34 → "RIVR-CD34".
 */
static void rivr_ble_set_device_name(void)
{
    char name[16];
    snprintf(name, sizeof(name), "RIVR-%04X",
             (unsigned)(g_my_node_id & 0xFFFFu));
    ble_svc_gap_device_name_set(name);
    RIVR_LOGI(TAG, "BLE device name: %s", name);
}

/**
 * @brief Start BLE advertising (called from ble_hs_on_sync and on reconnect).
 *
 * Sets full ADV payload: device name + complete list of 128-bit service UUIDs.
 * Interval 500–1000 ms (background scanning; optimise for battery if needed).
 *
 * No-ops if BLE is currently inactive (deactivated by timeout or user).
 */
static void rivr_ble_start_adv(void)
{
    if (!s_ble_active) {
        RIVR_LOGD(TAG, "rivr_ble_start_adv: BLE inactive — skipping");
        return;
    }

    int rc;

    /* ── Advertising fields ── */
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Full device name in advertising packet */
    const char *name = ble_svc_gap_device_name();
    fields.name          = (uint8_t *)name;
    fields.name_len      = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    /* Include the Rivr service UUID so scanners can identify the node */
    fields.uuids128             = &s_adv_svc_uuid;
    fields.num_uuids128         = 1u;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        /* ADV packet may be too large; fall back to name-only payload */
        RIVR_LOGW(TAG, "ble_gap_adv_set_fields failed (%d) — retrying name-only", rc);
        memset(&fields, 0, sizeof(fields));
        fields.flags             = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name              = (uint8_t *)name;
        fields.name_len          = (uint8_t)strlen(name);
        fields.name_is_complete  = 1;
        rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_adv_set_fields (fallback) failed: %d", rc);
            return;
        }
    }

    /* ── Advertising parameters ── */
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* undirected connectable */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* general discoverable   */
    /* 500–1000 ms advertising interval in units of 0.625 ms */
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(500);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(1000);

    /* ble_gap_adv_start fires rivr_ble_gap_event on connection / timeout */
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, rivr_ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        /* BLE_HS_EALREADY just means we were already advertising — fine. *
         * Any other error warrants a log entry.                          */
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        RIVR_LOGI(TAG, "BLE advertising started");
    }
}

/* ── GAP event handler (called from NimBLE host task) ───────────────────── */

/**
 * @brief NimBLE GAP event callback: connection, disconnect, MTU, subscribe.
 *
 * Executes in the NimBLE host task context.
 * Writes to s_conn_handle (volatile) — safe because main-loop reads are
 * naturally tolerant of one-tick stale reads on 32-bit Xtensa.
 */
static int rivr_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            /* ── Successfully connected ── */
            s_conn_handle = event->connect.conn_handle;
            g_rivr_metrics.ble_connections++;
            RIVR_LOGI(TAG, "BLE connected (conn_handle=0x%04x)",
                      (unsigned)s_conn_handle);
        } else {
            /* ── Connection attempt failed — restart advertising ── */
            RIVR_LOGW(TAG, "BLE connect failed (status=%d) — restarting adv",
                      event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            rivr_ble_start_adv();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        RIVR_LOGI(TAG, "BLE disconnected (conn_handle=0x%04x reason=%d)",
                  (unsigned)event->disconnect.conn.conn_handle,
                  event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Restart advertising so the phone can reconnect while BLE window is open */
        rivr_ble_start_adv();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        RIVR_LOGD(TAG, "BLE advertising complete (reason=%d)",
                  event->adv_complete.reason);
        /* Restart if still active (e.g. timed out on the ADV side) */
        if (s_ble_active) {
            rivr_ble_start_adv();
        }
        break;

    case BLE_GAP_EVENT_MTU:
        RIVR_LOGI(TAG, "BLE MTU update (conn=0x%04x mtu=%u)",
                  (unsigned)event->mtu.conn_handle,
                  (unsigned)event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        RIVR_LOGD(TAG, "BLE subscribe (conn=0x%04x attr=0x%04x notify=%d)",
                  (unsigned)event->subscribe.conn_handle,
                  (unsigned)event->subscribe.attr_handle,
                  event->subscribe.cur_notify);
        break;

    default:
        break;
    }

    return 0;
}

/* ── NimBLE host lifecycle callbacks ─────────────────────────────────────── */

/**
 * @brief Called by NimBLE when the host is synchronised with the controller.
 *
 * This is the correct place to start advertising.  Fires ~200 ms after
 * nimble_port_freertos_init() and after any subsequent host reset.
 */
static void rivr_ble_on_sync(void)
{
    int rc;

    /* Ensure a valid public address is available */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr: %d", rc);
        return;
    }

    s_host_synced = true;
    RIVR_LOGI(TAG, "BLE host synced — starting advertising");
    rivr_ble_set_device_name();
    rivr_ble_start_adv();
}

/**
 * @brief Called by NimBLE on host reset (controller crash, etc.).
 *
 * Log the reason and wait for re-sync (on_sync will fire again).
 */
static void rivr_ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset (reason=%d) — awaiting re-sync", reason);
    s_host_synced = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

/* ── NimBLE host task ────────────────────────────────────────────────────── */

/**
 * @brief FreeRTOS task entry point for the NimBLE host.
 *
 * nimble_port_run() blocks until nimble_port_stop() is called.
 * After return it calls nimble_port_freertos_deinit() and deletes itself.
 */
static void ble_host_task(void *param)
{
    (void)param;
    RIVR_LOGI(TAG, "BLE host task started");
    nimble_port_run();           /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void rivr_ble_init(void)
{
    int rc;

    RIVR_LOGI(TAG, "rivr_ble_init: initialising NimBLE stack");

    /* ── 1. Initialise the NimBLE porting layer ── */
    rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d — BLE disabled", rc);
        return;
    }

    /* ── 2. Configure the NimBLE host ── */
    ble_hs_cfg.reset_cb       = rivr_ble_on_reset;
    ble_hs_cfg.sync_cb        = rivr_ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;  /* RAM store */
    /* No security manager callbacks for initial no-pairing version */

    /* ── 3. Register standard GAP + GATT services ── */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* ── 4. Register the Rivr UART service ── */
    rc = rivr_ble_service_register();
    if (rc != 0) {
        ESP_LOGE(TAG, "rivr_ble_service_register failed: %d — BLE disabled", rc);
        return;
    }

    /* ── 5. Mark BLE as active in the BOOT_WINDOW mode ── */
    s_ble_active   = true;
    s_activate_ms  = 0u;   /* tb_millis() is valid now; will be set in tick */
    s_timeout_ms   = BLE_BOOT_WINDOW_MS;
    s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;

    /* ── 6. Start the NimBLE host FreeRTOS task ── */
    /* The task calls nimble_port_run() which processes all NimBLE events.
     * Stack depth 4 KB is sufficient for NimBLE with simple GATT usage.    */
    nimble_port_freertos_init(ble_host_task);

    RIVR_LOGI(TAG, "NimBLE host task started; BLE active (%lu ms window)",
              (unsigned long)BLE_BOOT_WINDOW_MS);
}

void rivr_ble_tick(uint32_t now_ms)
{
    /* Capture activate_ms on the first tick after init */
    if (s_activate_ms == 0u && s_ble_active) {
        s_activate_ms = now_ms;
    }

    if (!s_ble_active) return;
    if (s_timeout_ms == 0u) return; /* APP_REQUESTED → no timeout */

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
        s_timeout_ms = BLE_BOOT_WINDOW_MS;
        break;
    case RIVR_BLE_MODE_BUTTON:
        s_timeout_ms = BLE_BUTTON_WINDOW_MS;
        break;
    case RIVR_BLE_MODE_APP_REQUESTED:
        s_timeout_ms = 0u;  /* no timeout */
        break;
    default:
        s_timeout_ms = BLE_BOOT_WINDOW_MS;
        break;
    }

    s_ble_active  = true;
    s_activate_ms = 0u;  /* gets set on next tick */

    RIVR_LOGI(TAG, "BLE activated (mode=%d timeout=%lu ms)",
              (int)mode, (unsigned long)s_timeout_ms);

    /* If the host is already synced, start advertising immediately */
    if (s_host_synced && !ble_gap_adv_active()) {
        rivr_ble_start_adv();
    }
}

void rivr_ble_deactivate(void)
{
    if (!s_ble_active) return;

    s_ble_active = false;

    /* Stop advertising if it is running */
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
        RIVR_LOGI(TAG, "BLE advertising stopped");
    }

    /* Terminate any active connection gracefully */
    uint16_t ch = s_conn_handle;
    if (ch != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(ch, BLE_ERR_REM_USER_CONN_TERM);
        /* s_conn_handle is cleared by the GAP DISCONNECT event */
        RIVR_LOGI(TAG, "BLE connection terminated (conn_handle=0x%04x)", (unsigned)ch);
    }
}

bool rivr_ble_is_active(void)
{
    return s_ble_active;
}

bool rivr_ble_is_connected(void)
{
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

uint16_t rivr_ble_conn_handle(void)
{
    return s_conn_handle;
}

#endif /* RIVR_FEATURE_BLE */
