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
 *  Primary ADV carries the short discoverable payload (flags + name).
 *  Scan response carries the 128-bit Rivr service UUID for scanner identification.
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
#include "esp_random.h"

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

#if RIVR_BLE_PASSKEY != 0
#define RIVR_BLE_DEFAULT_SENTINEL_PASSKEY 123456UL
#endif

/* ── Module state ────────────────────────────────────────────────────────── *
 * s_conn_handle: written by NimBLE task in gap_event_cb,                   *
 *               read by main-loop task in is_connected()/service_notify(). *
 * volatile provides single-field torn-read protection on 32-bit Xtensa;    *
 * a full mutex is not necessary for this diagnostic/gate usage.            */

/** Current connection handle; BLE_HS_CONN_HANDLE_NONE = no client.
 *  With RIVR_BLE_PASSKEY != 0 this is only set after successful encryption
 *  (BLE_GAP_EVENT_ENC_CHANGE), not immediately on connect. */
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

#if RIVR_BLE_PASSKEY != 0
/** GATT connection handle for an in-progress (unencrypted) pairing session.
 *  Promoted to s_conn_handle once BLE_GAP_EVENT_ENC_CHANGE fires with
 *  status 0.  BLE_HS_CONN_HANDLE_NONE when no unencrypted session exists. */
static volatile uint16_t s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#endif

/** True while BLE stack is advertising or has an active connection. */
static volatile bool s_ble_active = false;

/** Monotonic ms at last rivr_ble_activate() call. */
static uint32_t s_activate_ms = 0u;

/** Auto-deactivation timeout in ms; 0 = no timeout (APP_REQUESTED mode). */
static uint32_t s_timeout_ms  = BLE_BOOT_WINDOW_MS;

/** Prevents rivr_start_adv() from running before the host is synced. */
static volatile bool s_host_synced = false;

#if RIVR_BLE_PASSKEY != 0
/** Active 6-digit passkey for this boot session. */
static uint32_t s_active_passkey = 0u;
#endif

/* ── Service UUID (used in advertising payload) ───────────────────────────── *
 * 6E400001-B5A3-F393-E0A9-E50E24DCCA9E in little-endian byte order.         */
static const ble_uuid128_t s_adv_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

#if RIVR_BLE_PASSKEY != 0
static uint32_t rivr_ble_choose_passkey(void)
{
#if RIVR_FEATURE_DISPLAY
    if ((uint32_t)RIVR_BLE_PASSKEY == RIVR_BLE_DEFAULT_SENTINEL_PASSKEY) {
        return 100000u + (esp_random() % 900000u);
    }
#endif
    return (uint32_t)RIVR_BLE_PASSKEY;
}
#endif

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
 * Keeps the primary ADV packet compact by advertising the local name there and
 * placing the 128-bit service UUID in the scan response.
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

    /* ── Resolve own address type ──────────────────────────────────────────── *
     * Let NimBLE infer the correct address type based on what the chip has.  *
     * Do NOT hardcode BLE_OWN_ADDR_PUBLIC: dev boards with no burned-in       *
     * IEEE BD_ADDR fall back to a random static address in NVS, and passing  *
     * BLE_OWN_ADDR_PUBLIC in that case causes ble_gap_adv_start to either    *
     * fail silently or advertise in a way Android cannot connect to.         */
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d — cannot advertise", rc);
        return;
    }

    /* ── Advertising fields ──
     * Keep primary ADV within the 31-byte limit: flags + local name only.
     * Put the 128-bit NUS service UUID into the scan response.
     */
    struct ble_hs_adv_fields adv_fields;
    struct ble_hs_adv_fields rsp_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    const char *name = ble_svc_gap_device_name();
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = (uint8_t)strlen(name);
    adv_fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    rsp_fields.uuids128 = &s_adv_svc_uuid;
    rsp_fields.num_uuids128 = 1u;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        /* Fail-open: keep advertising name-only if scan response cannot be set.
         * This preserves discoverability in RF Connect / companion even when
         * a controller/NimBLE quirk rejects scan-response updates. */
        RIVR_LOGW(TAG, "ble_gap_adv_rsp_set_fields failed (%d) - continuing name-only", rc);
    }

    /* ── Advertising parameters ── */
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* undirected connectable */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* general discoverable   */
    /* 100–200 ms advertising interval during active window (faster connection) */
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(100);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(200);

    /* ble_gap_adv_start fires rivr_ble_gap_event on connection / timeout */
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, rivr_ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        /* BLE_HS_EALREADY just means we were already advertising — fine. *
         * Any other error warrants a log entry.                          */
        ESP_LOGE(TAG, "ble_gap_adv_start (addr_type=%d) failed: %d",
                 (int)own_addr_type, rc);
    } else {
        RIVR_LOGI(TAG, "BLE advertising started (own_addr_type=%d)", (int)own_addr_type);
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
#if RIVR_BLE_PASSKEY != 0
            /* With MITM bonding enabled, do NOT expose the connection until
             * encryption is established.  s_conn_handle stays NONE until
             * BLE_GAP_EVENT_ENC_CHANGE fires with authenticated=1.          */
            s_pending_conn_handle = event->connect.conn_handle;
            g_rivr_metrics.ble_connections++;
            RIVR_LOGI(TAG, "BLE connected (conn_handle=0x%04x) — initiating security",
                      (unsigned)s_pending_conn_handle);
            /* NimBLE's auto-generated CCCD does not inherit *_AUTHEN flags
             * from the characteristic value, so subscribing to notifications
             * is not a reliable way to trigger pairing.  Start security here
             * so Android gets the pairing / re-encryption flow immediately. */
            int sec_rc = ble_gap_security_initiate(s_pending_conn_handle);
            if (sec_rc != 0 && sec_rc != BLE_HS_EALREADY) {
                RIVR_LOGW(TAG, "BLE security initiate failed (rc=%d)", sec_rc);
            }
#else
            s_conn_handle = event->connect.conn_handle;
            g_rivr_metrics.ble_connections++;
            RIVR_LOGI(TAG, "BLE connected (conn_handle=0x%04x)",
                      (unsigned)s_conn_handle);
#endif
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
#if RIVR_BLE_PASSKEY != 0
        s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#endif
        /* Restart advertising so the phone can reconnect while BLE window is open */
        rivr_ble_start_adv();
        break;

#if RIVR_BLE_PASSKEY != 0
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status == 0) {
            /* Verify the bond is MITM-authenticated, not just encrypted.
             * A stale Just-Works bond re-encryption gives status=0 but
             * sec_state.authenticated=0 — we must reject it so Android
             * tears down the bond and starts fresh MITM pairing.          */
            struct ble_gap_conn_desc desc;
            int authen = (ble_gap_conn_find(event->enc_change.conn_handle,
                                            &desc) == 0)
                         && desc.sec_state.authenticated;
            if (authen) {
                s_conn_handle = s_pending_conn_handle;
                RIVR_LOGI(TAG,
                          "BLE encrypted & authenticated (conn_handle=0x%04x) — ready",
                          (unsigned)s_conn_handle);
            } else {
                /* Stale Just-Works bond — delete it from NVS so the next
                 * connection attempt finds no LTK.  Android will then get
                 * BT_HCI_ERR_PIN_OR_KEY_MISSING, clear its own cached bond,
                 * and start a fresh MITM pairing with passkey entry.
                 * Terminate with a normal user-disconnect reason so Android
                 * does NOT show "Is device ready to pair?" error.          */
                RIVR_LOGW(TAG,
                          "BLE link not MITM-authenticated — deleting stale bond");
                struct ble_gap_conn_desc desc2;
                if (ble_gap_conn_find(event->enc_change.conn_handle,
                                      &desc2) == 0) {
                    ble_gap_unpair(&desc2.peer_id_addr);
                }
                ble_gap_terminate(s_pending_conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
                s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
        } else {
            /* Pairing failed — disconnect and re-advertise */
            RIVR_LOGW(TAG, "BLE encryption failed (status=%d) — disconnecting",
                      event->enc_change.status);
            ble_gap_terminate(s_pending_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        break;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey;
        memset(&pkey, 0, sizeof(pkey));
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            /* Passkey Entry: we display, Android types.  Inject our static
             * passkey — Android will show an input field for it.          */
            pkey.action  = BLE_SM_IOACT_DISP;
            pkey.passkey = s_active_passkey;
            RIVR_LOGI(TAG, "BLE pairing: Passkey Entry — show %06lu on phone",
                      (unsigned long)pkey.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            /* DisplayOnly should normally force Passkey Entry on Android.
             * If a stack still requests Numeric Comparison, accept it so the
             * link can progress rather than failing outright. */
            pkey.action        = BLE_SM_IOACT_NUMCMP;
            pkey.numcmp_accept = 1;
            RIVR_LOGW(TAG, "BLE pairing: unexpected Numeric Comparison %06lu — auto-accepted",
                      (unsigned long)event->passkey.params.numcmp);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else {
            RIVR_LOGW(TAG, "BLE pairing: unexpected PASSKEY_ACTION %d — ignoring",
                      (int)event->passkey.params.action);
        }
        break;
    }
#endif

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
#if RIVR_BLE_PASSKEY != 0
    s_pending_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#endif
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
    ble_hs_cfg.reset_cb        = rivr_ble_on_reset;
    ble_hs_cfg.sync_cb         = rivr_ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

#if RIVR_BLE_PASSKEY != 0
    s_active_passkey = rivr_ble_choose_passkey();

    /* MITM-protected bonding using DisplayOnly IO capability.
     *
     * IO cap selection rationale (BT Core Vol 3, Part H §2.3.5.1):
     *   Android KeyboardDisplay + ESP32 DisplayOnly → Passkey Entry
     *   Android DisplayYesNo   + ESP32 DisplayOnly → Passkey Entry / OS
     *                                                mediated passkey UI
     *
     * This matches the MeshCore UX: show a 6-digit PIN on the node and let
     * Android ask the user to enter it during pairing.                    */
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;   /* LE Secure Connections           */
    /* Distribute encryption + identity keys both ways so IRK-based RPA     *
     * resolution works after re-bonding with a new phone.                  */
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    RIVR_LOGI(TAG, "BLE security: MITM passkey bonding enabled (PIN=%06lu)",
              (unsigned long)s_active_passkey);
#else
    RIVR_LOGI(TAG, "BLE security: open (no pairing)");
#endif

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
#if RIVR_BLE_PASSKEY != 0
    s_timeout_ms   = 0u;   /* MeshCore-style secure BLE: stay available */
#else
    s_timeout_ms   = BLE_BOOT_WINDOW_MS;
#endif
    s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;

    /* ── 6. Start the NimBLE host FreeRTOS task ── */
    /* The task calls nimble_port_run() which processes all NimBLE events.
     * Stack depth 4 KB is sufficient for NimBLE with simple GATT usage.    */
    nimble_port_freertos_init(ble_host_task);

    if (s_timeout_ms == 0u) {
        RIVR_LOGI(TAG, "NimBLE host task started; BLE active without timeout");
    } else {
        RIVR_LOGI(TAG, "NimBLE host task started; BLE active (%lu ms window)",
                  (unsigned long)BLE_BOOT_WINDOW_MS);
    }
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

uint32_t rivr_ble_passkey(void)
{
#if RIVR_BLE_PASSKEY != 0
    return s_active_passkey;
#else
    return 0u;
#endif
}

#endif /* RIVR_FEATURE_BLE */
