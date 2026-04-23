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

#include <Adafruit_LittleFS.h>
#include <Arduino.h>
#include <bluefruit.h>
#include <InternalFileSystem.h>
#include <string.h>
#include <stdio.h>
#include <nrf_soc.h>

extern "C" {
#include "rivr_ble_frag.h"
#include "radio_sx1262.h"   /* rf_rx_ringbuf, rf_rx_frame_t, RF_MAX_PAYLOAD_LEN */
#include "rivr_log.h"
#include "rivr_metrics.h"
#include "timebase.h"
#include "ringbuf.h"

/* g_my_node_id — set by main before rivr_ble_init() */
extern uint32_t g_my_node_id;
} /* extern "C" */

#define TAG "BLE_NRF52"
#define RIVR_BLE_BOND_DIR_PRPH "/adafruit/bond_prph"

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
static volatile uint16_t s_pending_conn_handle = 0xFFFFu;
static volatile bool     s_ble_active  = false;
static uint32_t          s_activate_ms = 0u;
static uint32_t          s_timeout_ms  = BLE_BOOT_WINDOW_MS;
static bool              s_has_bond    = false;
static rivr_ble_frag_rx_t s_rx_frag;
static uint8_t           s_tx_frag_msg_id = 0u;
#if RIVR_BLE_PASSKEY != 0
static uint32_t          s_active_passkey = 0u;
#endif

/* ── Internal helpers ────────────────────────────────────────────────────── */

static bool rivr_ble_bond_store_has_entries(void)
{
    InternalFS.begin();

    Adafruit_LittleFS_Namespace::File dir(
        RIVR_BLE_BOND_DIR_PRPH,
        Adafruit_LittleFS_Namespace::FILE_O_READ,
        InternalFS);
    if (!dir) {
        return false;
    }

    Adafruit_LittleFS_Namespace::File file(InternalFS);
    while ((file = dir.openNextFile(Adafruit_LittleFS_Namespace::FILE_O_READ))) {
        file.close();
        dir.close();
        return true;
    }

    dir.close();
    return false;
}

#if RIVR_BLE_PASSKEY != 0
#if RIVR_BLE_RANDOM_PASSKEY
static uint32_t rivr_ble_random_u32(void)
{
    uint8_t bytes[4];
    if (sd_rand_application_vector_get(bytes, sizeof(bytes)) == NRF_SUCCESS) {
        return ((uint32_t)bytes[0]) |
               ((uint32_t)bytes[1] << 8) |
               ((uint32_t)bytes[2] << 16) |
               ((uint32_t)bytes[3] << 24);
    }

    return ((uint32_t)micros() << 12) ^ (uint32_t)millis() ^ g_my_node_id;
}
#endif

static uint32_t rivr_ble_choose_passkey(void)
{
#if RIVR_BLE_RANDOM_PASSKEY
    return 100000u + (rivr_ble_random_u32() % 900000u);
#else
    return (uint32_t)RIVR_BLE_PASSKEY;
#endif
}

static void security_complete_cb(uint16_t conn_handle, uint8_t auth_status)
{
    if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        s_has_bond = true;
        RIVR_LOGI(TAG, "pairing complete (conn_handle=0x%04x)", (unsigned)conn_handle);
        return;
    }

    RIVR_LOGW(TAG, "pairing failed (conn_handle=0x%04x auth_status=0x%02x)",
              (unsigned)conn_handle, (unsigned)auth_status);
    if (Bluefruit.connected(conn_handle)) {
        Bluefruit.disconnect(conn_handle);
    }
}

static void security_secured_cb(uint16_t conn_handle)
{
    BLEConnection *conn = Bluefruit.Connection(conn_handle);
    if (conn == NULL) {
        return;
    }

    s_conn_handle = conn_handle;
    s_pending_conn_handle = 0xFFFFu;
    if (conn->bonded()) {
        s_has_bond = true;
    }
    conn->requestMtuExchange(RIVR_BLE_ATT_PREFERRED_MTU);
    RIVR_LOGI(TAG, "link secured (conn_handle=0x%04x mtu=%u payload=%u)",
              (unsigned)conn_handle,
              (unsigned)conn->getMtu(),
              (unsigned)rivr_ble_link_payload_limit());
}
#endif

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
    RIVR_LOGI(TAG, "connected, conn_handle=0x%04x", (unsigned)conn_handle);
    g_rivr_metrics.ble_connections++;

    BLEConnection *conn = Bluefruit.Connection(conn_handle);
    if (conn != NULL) {
        conn->requestMtuExchange(RIVR_BLE_ATT_PREFERRED_MTU);
    }

#if RIVR_BLE_PASSKEY != 0
    s_pending_conn_handle = conn_handle;
    if (conn != NULL) {
        conn->requestPairing();
    }
#else
    s_conn_handle = conn_handle;
#endif
}

static void disconnect_cb(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    s_conn_handle = 0xFFFFu;
    s_pending_conn_handle = 0xFFFFu;
    rivr_ble_frag_reset(&s_rx_frag);
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
    const uint8_t *payload = NULL;
    uint16_t payload_len = 0u;
    rivr_ble_frag_rx_result_t frag_result;
    rf_rx_frame_t frame;

    (void)conn_handle;
    (void)chr;

    frag_result = rivr_ble_frag_ingest(&s_rx_frag, data, len,
                                       &payload, &payload_len);
    if (frag_result == RIVR_BLE_FRAG_RX_INVALID) {
        RIVR_LOGW(TAG, "rx: invalid fragment stream");
        g_rivr_metrics.ble_errors++;
        return;
    }
    if (frag_result == RIVR_BLE_FRAG_RX_INCOMPLETE) {
        return;
    }

    if (payload_len == 0u || payload_len > RF_MAX_PAYLOAD_LEN) {
        RIVR_LOGW(TAG, "rx: frame length %u out of range (max=%u)",
                  (unsigned)payload_len, (unsigned)RF_MAX_PAYLOAD_LEN);
        g_rivr_metrics.ble_errors++;
        return;
    }

    if (rivr_ble_companion_handle_rx(payload, payload_len)) {
        return;
    }

    memset(&frame, 0, sizeof(frame));
    memcpy(frame.data, payload, payload_len);
    frame.len        = (uint8_t)payload_len;
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

static bool rivr_ble_service_notify_one(void *ctx, const uint8_t *data, uint16_t len)
{
    uint16_t conn_handle = *(const uint16_t *)ctx;

    return s_nus_tx.notify(conn_handle, data, len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * rivr_ble.h public API — nRF52 implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

void rivr_ble_init(void)
{
    char name[20];
    snprintf(name, sizeof(name), "RIVR-%04lX", (unsigned long)(g_my_node_id & 0xFFFFu));

    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    Bluefruit.setName(name);
    Bluefruit.setTxPower(8);   /* +8 dBm — S140 SoftDevice maximum, matches ESP32 target */
    Bluefruit.Periph.setConnIntervalMS(15u, 30u);
    s_has_bond = rivr_ble_bond_store_has_entries();

    Bluefruit.Periph.setConnectCallback(connect_cb);
    Bluefruit.Periph.setDisconnectCallback(disconnect_cb);

#if RIVR_BLE_PASSKEY != 0
    /* MITM-protected bonding: node displays passkey, user enters it on phone. */
    Bluefruit.Security.setIOCaps(true, false, false);   /* display-only */
    Bluefruit.Security.setMITM(true);
    Bluefruit.Security.setPairCompleteCallback(security_complete_cb);
    Bluefruit.Security.setSecuredCallback(security_secured_cb);
    {
        /* BLESecurity::setPIN() takes a 6-character ASCII string (no NUL in
         * the 6 bytes), matching the BLE_GAP_PASSKEY_LEN requirement. */
        char pk_str[7];
        s_active_passkey = rivr_ble_choose_passkey();
        snprintf(pk_str, sizeof(pk_str), "%06lu", (unsigned long)s_active_passkey);
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
    s_conn_handle = 0xFFFFu;
    s_pending_conn_handle = 0xFFFFu;
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

uint16_t rivr_ble_link_payload_limit(void)
{
    BLEConnection *conn;
    uint16_t mtu;

    if (s_conn_handle == 0xFFFFu || !Bluefruit.connected(s_conn_handle)) {
        return 0u;
    }

    conn = Bluefruit.Connection(s_conn_handle);
    if (conn == NULL) {
        return 0u;
    }

    mtu = conn->getMtu();
    if (mtu <= 3u) {
        return 0u;
    }
    if (mtu > RIVR_BLE_ATT_PREFERRED_MTU) {
        mtu = RIVR_BLE_ATT_PREFERRED_MTU;
    }
    return (uint16_t)(mtu - 3u);
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
    return s_has_bond;
}

int rivr_ble_clear_bonds(void)
{
    if (s_conn_handle != 0xFFFFu) {
        Bluefruit.disconnect(s_conn_handle);
        s_conn_handle = 0xFFFFu;
    }
    Bluefruit.Periph.clearBonds();
    s_has_bond = false;
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

bool rivr_ble_service_notify(uint16_t conn_handle,
                             const uint8_t *data, uint8_t len)
{
    uint16_t limit = rivr_ble_link_payload_limit();

    if (s_conn_handle == 0xFFFFu) return false;
    if (!data || len == 0u) return false;
    if (limit == 0u) {
        g_rivr_metrics.ble_errors++;
        RIVR_LOGW(TAG, "notify skipped: len=%u payload_limit=%u",
                  (unsigned)len, (unsigned)limit);
        return false;
    }

    if (rivr_ble_frag_send(data, len, limit, &s_tx_frag_msg_id,
                           rivr_ble_service_notify_one, &conn_handle)) {
        g_rivr_metrics.ble_tx_frames++;
        return true;
    }

    g_rivr_metrics.ble_errors++;
    RIVR_LOGD(TAG, "notify failed for conn_handle=0x%04x", (unsigned)conn_handle);
    return false;
}

#endif /* RIVR_FEATURE_BLE && RIVR_PLATFORM_NRF52840 */
