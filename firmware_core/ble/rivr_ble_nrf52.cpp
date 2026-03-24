/**
 * @file  firmware_core/ble/rivr_ble_nrf52.cpp
 * @brief Rivr BLE transport — Adafruit Bluefruit (nRF52840 SoftDevice) backend.
 *
 * Implements the same public API as rivr_ble.c (Bluedroid / ESP32) but uses
 * the Adafruit nRF52 Arduino BSP's Bluefruit library, which wraps Nordic's
 * S140 SoftDevice.
 *
 * GATT SERVICE
 * ────────────
 * Nordic UART Service (NUS) UUIDs — identical to the ESP32 implementation so
 * the Rivr Companion app can connect to both platforms without any changes:
 *
 *   Service UUID:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX char UUID:  6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (Write / Write-NR)
 *   TX char UUID:  6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (Notify)
 *
 * THREADING (FreeRTOS / SoftDevice)
 * ───────────────────────────────────
 *  BLE callbacks execute in the SD event handler context (high-priority task).
 *  rf_rx_ringbuf is SPSC:
 *    producer = rx_write_cb (SD callback context)
 *    consumer = rivr_main_task main loop (sources_rf_rx_drain)
 *
 *  rivr_ble_tick(), rivr_ble_activate(), rivr_ble_deactivate() and
 *  rivr_ble_service_notify() MUST be called from the rivr_main_task only.
 *
 * FEATURE FLAG
 * ────────────
 *  Only compiled when RIVR_FEATURE_BLE=1 AND RIVR_PLATFORM_NRF52840=1.
 *  The ESP32 Bluedroid implementation (rivr_ble.c) is never included in nRF52
 *  builds, so there is no symbol clash.
 */

#include "rivr_ble.h"
#include "rivr_ble_companion.h"
#include "rivr_ble_service.h"
#include "rivr_config.h"

#if RIVR_FEATURE_BLE && RIVR_PLATFORM_NRF52840

#include <Arduino.h>
#include <bluefruit.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "radio_sx1262.h"   /* rf_rx_ringbuf, rf_rx_frame_t, RF_MAX_PAYLOAD_LEN */
#include "rivr_log.h"
#include "rivr_metrics.h"
#include "timebase.h"
#include "ringbuf.h"

/* g_my_node_id — set by main before rivr_ble_init() */
extern uint32_t g_my_node_id;
} /* extern "C" */

#define TAG "BLE_NRF52"

/* ── NUS GATT service & characteristics ──────────────────────────────────── */

/* 128-bit UUIDs as string-literal — Bluefruit parses these via BLEUuid.     */
#define NUS_SVC_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static BLEService        s_nus_svc(NUS_SVC_UUID);
static BLECharacteristic s_nus_rx(NUS_RX_UUID);   /* phone → node  */
static BLECharacteristic s_nus_tx(NUS_TX_UUID);   /* node  → phone */

/* ── State ───────────────────────────────────────────────────────────────── */

static volatile uint16_t s_conn_handle = 0xFFFFu;
static volatile bool     s_ble_active  = false;
static uint32_t          s_activate_ms = 0u;
static uint32_t          s_timeout_ms  = BLE_BOOT_WINDOW_MS;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void start_adv(void)
{
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(s_nus_svc);
    Bluefruit.ScanResponse.addName();

    /* Does not auto-restart on disconnect — disconnect_cb() handles that so
     * we can suppress restart when BLE has been deactivated. */
    Bluefruit.Advertising.restartOnDisconnect(false);

    /* ~100 ms advertising interval in 0.625 ms units = 160 */
    Bluefruit.Advertising.setInterval(160, 160);

    /* Advertise indefinitely (0 = no timeout). rivr_ble_tick() will call
     * rivr_ble_deactivate() when the window expires. */
    Bluefruit.Advertising.start(0);
    RIVR_LOGI(TAG, "advertising started");
}

/* ── SoftDevice callbacks ────────────────────────────────────────────────── */

static void connect_cb(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    RIVR_LOGI(TAG, "connected, conn_handle=0x%04x", (unsigned)conn_handle);
    g_rivr_metrics.ble_connections++;
}

static void disconnect_cb(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    s_conn_handle = 0xFFFFu;
    rivr_ble_companion_on_disconnect();
    RIVR_LOGI(TAG, "disconnected, reason=0x%02x", (unsigned)reason);

    /* Restart advertising if still within the activation window. */
    if (s_ble_active) {
        start_adv();
    }
}

/**
 * @brief Write callback — phone sent a Rivr frame over BLE.
 *
 * Called from the SoftDevice event-handler context (high-priority FreeRTOS
 * task).  The only shared state touched here is rf_rx_ringbuf, which is an
 * SPSC ringbuf whose producer side is exclusively this callback.
 */
static void rx_write_cb(uint16_t conn_handle,
                        BLECharacteristic *chr,
                        uint8_t *data, uint16_t len)
{
    (void)conn_handle;
    (void)chr;

    if (len == 0u || len > RF_MAX_PAYLOAD_LEN) {
        RIVR_LOGW(TAG, "rx: frame length %u out of range (max=%u)",
                  (unsigned)len, (unsigned)RF_MAX_PAYLOAD_LEN);
        g_rivr_metrics.ble_errors++;
        return;
    }

    if (rivr_ble_companion_handle_rx(data, len)) {
        return;
    }

    rf_rx_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    memcpy(frame.data, data, len);
    frame.len        = (uint8_t)len;
    frame.rssi_dbm   = 0;
    frame.snr_db     = 0;
    frame.rx_mono_ms = tb_millis();
    frame.from_id    = 0u;
    frame.iface      = 1u;   /* RIVR_IFACE_BLE */

    if (!rb_try_push(&rf_rx_ringbuf, &frame)) {
        RIVR_LOGW(TAG, "rx: rf_rx_ringbuf full — frame dropped");
        g_rivr_metrics.ble_errors++;
        return;
    }

    g_rivr_metrics.ble_rx_frames++;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * rivr_ble.h public API — nRF52 implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

void rivr_ble_init(void)
{
    char name[20];
    snprintf(name, sizeof(name), "RIVR-%08lX", (unsigned long)g_my_node_id);

    Bluefruit.begin();
    Bluefruit.setName(name);
    Bluefruit.setTxPower(4);   /* +4 dBm — reasonable for peripheral use */

    Bluefruit.Periph.setConnectCallback(connect_cb);
    Bluefruit.Periph.setDisconnectCallback(disconnect_cb);

#if RIVR_BLE_PASSKEY != 0
    /* MITM-protected bonding: node displays passkey, user enters it on phone. */
    Bluefruit.Security.setIOCaps(true, false, false);   /* display-only */
    Bluefruit.Security.setMITM(true);
    {
        /* BLESecurity::setPIN() takes a 6-character ASCII string (no NUL in
         * the 6 bytes), matching the BLE_GAP_PASSKEY_LEN requirement. */
        char pk_str[7];
        snprintf(pk_str, sizeof(pk_str), "%06lu", (unsigned long)(RIVR_BLE_PASSKEY));
        Bluefruit.Security.setPIN(pk_str);
        RIVR_LOGI(TAG, "BLE security: MITM passkey bonding enabled (PIN=%s)",
                  pk_str);
    }
#else
    RIVR_LOGI(TAG, "BLE security: open (no pairing)");
#endif

    /* ── GATT service setup ─────────────────────────────────────────────── */
    s_nus_svc.begin();

    /* RX char: phone → node (Write + Write-Without-Response) */
    s_nus_rx.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
#if RIVR_BLE_PASSKEY != 0
    s_nus_rx.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
#else
    s_nus_rx.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
#endif
    s_nus_rx.setFixedLen(0);
    s_nus_rx.setMaxLen(RF_MAX_PAYLOAD_LEN);
    s_nus_rx.setWriteCallback(rx_write_cb);
    s_nus_rx.begin();

    /* TX char: node → phone (Notify) */
    s_nus_tx.setProperties(CHR_PROPS_NOTIFY);
#if RIVR_BLE_PASSKEY != 0
    s_nus_tx.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_NO_ACCESS);
#else
    s_nus_tx.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
#endif
    s_nus_tx.setFixedLen(0);
    s_nus_tx.setMaxLen(RF_MAX_PAYLOAD_LEN);
    s_nus_tx.begin();

    /* ── Timing state ───────────────────────────────────────────────────── */
    s_ble_active  = true;
    s_activate_ms = 0u;
#if RIVR_BLE_PASSKEY != 0
    s_timeout_ms = 0u;   /* passkey builds stay on until explicitly deactivated */
#else
    s_timeout_ms = BLE_BOOT_WINDOW_MS;
#endif

    start_adv();

    if (s_timeout_ms == 0u) {
        RIVR_LOGI(TAG, "BLE active (no timeout)");
    } else {
        RIVR_LOGI(TAG, "BLE active (%lu ms window)", (unsigned long)s_timeout_ms);
    }
}

void rivr_ble_tick(uint32_t now_ms)
{
    /* Record the first tick as the start of the activation window. */
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

    s_ble_active  = true;
    s_activate_ms = 0u;
    start_adv();
}

void rivr_ble_deactivate(void)
{
    s_ble_active = false;
    s_timeout_ms = 0u;

    Bluefruit.Advertising.stop();

    if (s_conn_handle != 0xFFFFu) {
        Bluefruit.disconnect(s_conn_handle);
        s_conn_handle = 0xFFFFu;
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
    return (uint32_t)RIVR_BLE_PASSKEY;
#else
    return 0u;
#endif
}

bool rivr_ble_has_bond(void)
{
    /* The Adafruit nRF52 BSP does not expose a bond count query directly.
     * Bond presence can be inferred: if a peer reconnects with encryption
     * without re-pairing, it has a stored bond.  For now return false;
     * callers use this only for UI hints.                                    */
    return false;
}

int rivr_ble_clear_bonds(void)
{
    if (s_conn_handle != 0xFFFFu) {
        Bluefruit.disconnect(s_conn_handle);
        s_conn_handle = 0xFFFFu;
    }
    Bluefruit.Periph.clearBonds();
    RIVR_LOGI(TAG, "BLE bonds cleared");
    if (s_ble_active) {
        start_adv();
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * rivr_ble_service.h API — nRF52 implementation
 *
 * On ESP32 the service module handles Bluedroid GATTS registration and the
 * GATT event dispatch loop.  On nRF52 the Bluefruit library manages all of
 * that automatically via BLECharacteristic callbacks registered in
 * rivr_ble_init().  These entry points therefore exist only to satisfy the
 * interface expected by rivr_iface_ble.c.
 * ═══════════════════════════════════════════════════════════════════════════ */

int rivr_ble_service_register(void)
{
    /* No-op: Bluefruit registers the service when s_nus_svc.begin() is called
     * inside rivr_ble_init().                                               */
    return 0;
}

void rivr_ble_service_notify(uint16_t conn_handle,
                              const uint8_t *data, uint8_t len)
{
    if (s_conn_handle == 0xFFFFu) return;
    if (!data || len == 0u) return;

    if (s_nus_tx.notify(conn_handle, data, (uint16_t)len)) {
        g_rivr_metrics.ble_tx_frames++;
    } else {
        g_rivr_metrics.ble_errors++;
        RIVR_LOGD(TAG, "notify failed for conn_handle=0x%04x", (unsigned)conn_handle);
    }
}

#endif /* RIVR_FEATURE_BLE && RIVR_PLATFORM_NRF52840 */
