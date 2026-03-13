/**
 * @file  rivr_ble.h
 * @brief Rivr BLE transport — public API.
 *
 * ARCHITECTURE
 * ────────────
 * BLE is a local edge interface only.  It does NOT introduce a second
 * protocol.  All communication uses the same binary Rivr packet frames
 * that travel over LoRa.
 *
 *   Rivr Packet API
 *   │
 *   ├── LoRa driver (SX1262 / SX1276)
 *   ├── BLE driver   ← this module
 *   └── (future: UART bridge, WiFi bridge)
 *
 * TRANSPORT INTEGRATION
 * ─────────────────────
 *  Phone → node :  Phone writes binary Rivr frame to the BLE RX
 *                  characteristic.  rivr_ble_service.c pushes it into
 *                  rf_rx_ringbuf, which is drained by sources_rf_rx_drain()
 *                  on the next main-loop tick — identical to LoRa ingest.
 *
 *  Node → phone :  Valid frames received from the mesh (passing protocol
 *                  decode, before dedupe) are forwarded to any connected
 *                  BLE client via TX-notify (rivr_ble_service_notify()).
 *
 * ACTIVATION MODES
 * ─────────────────
 *  BOOT_WINDOW    — BLE active for 120 s after boot (default)
 *  BUTTON         — User-triggered 5-minute window
 *  APP_REQUESTED  — Enabled via mesh command; stays on until deactivated
 *
 * THREADING MODEL
 * ───────────────
 *  NimBLE runs in its own FreeRTOS host task (nimble_port_freertos_init).
 *  BLE callbacks are called from the NimBLE host task.
 *
 *  rf_rx_ringbuf is an SPSC ring buffer:
 *    producer = NimBLE host task (BLE write callback)
 *    consumer = main-loop task  (sources_rf_rx_drain)
 *
 *  rivr_ble_service_notify() is called from the main-loop task, which is
 *  safe: ble_gatts_notify_custom() takes NimBLE's internal lock.
 *
 *  rivr_ble_tick() and rivr_ble_activate/deactivate() must ONLY be called
 *  from the main-loop task.  They are NOT thread-safe with the NimBLE task.
 *
 * SECURITY
 * ────────
 *  Initial version: no bonding, no encryption.  Pairing support is a
 *  future extension controlled by rivr_config.h.
 *
 * FEATURE FLAG
 * ────────────
 *  All code is compiled-out when RIVR_FEATURE_BLE == 0 (the default).
 *  Enable by building with -DRIVR_FEATURE_BLE=1 and a properly configured
 *  sdkconfig (see sdkconfig.ble at the project root).
 */

#ifndef RIVR_BLE_H
#define RIVR_BLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "rivr_config.h"

/* ── Feature guard — all symbols disappear when BLE is disabled ──────────── */
#if RIVR_FEATURE_BLE

/* ── Activation modes ────────────────────────────────────────────────────── */

/**
 * @brief BLE activation reason.
 *
 * Passed to rivr_ble_activate() to set the active timeout:
 *   BOOT_WINDOW   → 120 000 ms  (BLE_BOOT_WINDOW_MS)
 *   BUTTON        →  300 000 ms  (BLE_BUTTON_WINDOW_MS)
 *   APP_REQUESTED → 0 (no timeout — stays on until rivr_ble_deactivate())
 *   DISABLED      → no operation (only used as return value from getters)
 */
typedef enum {
    RIVR_BLE_MODE_BOOT_WINDOW    = 0,  /**< 120 s after boot (default)    */
    RIVR_BLE_MODE_BUTTON         = 1,  /**< 5-min window, user-triggered  */
    RIVR_BLE_MODE_APP_REQUESTED  = 2,  /**< Mesh command; no auto-timeout */
    RIVR_BLE_MODE_DISABLED       = 3,  /**< BLE off / not activated       */
} rivr_ble_mode_t;

/* ── Timing constants ────────────────────────────────────────────────────── */

/** How long BLE stays on after boot (ms). */
#define BLE_BOOT_WINDOW_MS     120000UL

/** How long a button-press BLE window lasts (ms). */
#define BLE_BUTTON_WINDOW_MS   300000UL

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialise NimBLE stack, register GATT service, start BLE host task.
 *
 * Must be called once from app_main() AFTER g_my_node_id is set and AFTER
 * platform_init() / timebase_init() have completed.
 *
 * On return, NimBLE is running in its own FreeRTOS task and will begin
 * advertising once the host sync callback fires (~few hundred ms later).
 * The activation mode is set to BLE_BOOT_WINDOW_MS by default.
 */
void rivr_ble_init(void);

/**
 * @brief Tick the BLE timeout state machine.
 *
 * Must be called once per main-loop iteration.  Compares the current
 * monotonic time against the activation deadline and calls
 * rivr_ble_deactivate() when the window expires.
 *
 * @param now_ms  Current tb_millis() value.
 */
void rivr_ble_tick(uint32_t now_ms);

/**
 * @brief Activate BLE advertising.
 *
 * Idempotent: calling while already active only resets the timeout.
 * MUST be called from the main-loop task (not from BLE callbacks or ISRs).
 *
 * @param mode  Activation reason — controls the auto-deactivation timeout.
 */
void rivr_ble_activate(rivr_ble_mode_t mode);

/**
 * @brief Deactivate BLE: stop advertising, disconnect any connected client.
 *
 * Safe to call even if BLE is already inactive.
 * MUST be called from the main-loop task.
 */
void rivr_ble_deactivate(void);

/* ── Status queries ──────────────────────────────────────────────────────── */

/**
 * @return true while BLE is advertising OR has an active connection.
 */
bool rivr_ble_is_active(void);

/**
 * @return true when a phone/client is connected.
 */
bool rivr_ble_is_connected(void);

/**
 * @return The connection handle of the current client, or
 *         BLE_HS_CONN_HANDLE_NONE (0xFFFF) when not connected.
 *         Only meaningful from the main-loop task (no NimBLE lock held).
 */
uint16_t rivr_ble_conn_handle(void);

#else  /* RIVR_FEATURE_BLE == 0 — compile everything to empty stubs ───────── */

typedef int rivr_ble_mode_t;
static inline void    rivr_ble_init(void)                           {}
static inline void    rivr_ble_tick(uint32_t now_ms)  { (void)now_ms; }
static inline void    rivr_ble_activate(rivr_ble_mode_t m){ (void)m; }
static inline void    rivr_ble_deactivate(void)                     {}
static inline bool    rivr_ble_is_active(void)   { return false; }
static inline bool    rivr_ble_is_connected(void){ return false; }
static inline uint16_t rivr_ble_conn_handle(void){ return 0xFFFFu; }

#endif /* RIVR_FEATURE_BLE */

#ifdef __cplusplus
}
#endif

#endif /* RIVR_BLE_H */
