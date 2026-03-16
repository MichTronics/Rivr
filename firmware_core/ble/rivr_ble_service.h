/**
 * @file  rivr_ble_service.h
 * @brief Rivr BLE GATT service — internal API.
 *
 * This module owns the Rivr UART-over-BLE service (Nordic NUS UUIDs):
 *
 *   Service UUID:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX char UUID:  6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (Write / Write-NR)
 *   TX char UUID:  6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (Notify)
 *
 * RESPONSIBILITIES
 * ────────────────
 *  rivr_ble_service.c registers the GATT attribute table with the active BLE
 *  stack and handles the RX/TX characteristic callbacks.
 *  rivr_ble.c calls rivr_ble_service_register() during stack init and
 *  rivr_ble_service_notify() to push frames to a connected client.
 *
 *  The write callback (phone → node) is handled entirely inside this module;
 *  it pushes validated frames into rf_rx_ringbuf without any locking, relying
 *  on the SPSC ring-buffer contract (BLE callback task = producer, main loop = consumer).
 */

#ifndef RIVR_BLE_SERVICE_H
#define RIVR_BLE_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "rivr_config.h"

#if RIVR_FEATURE_BLE

/**
 * @brief Register the Rivr GATT service application/profile.
 *
 * Must be called ONCE during BLE init, after GAP/GATTS callbacks were
 * registered with the ESP-IDF BT stack.
 *
 * @return 0 on success, ESP-IDF error code on failure.
 */
int rivr_ble_service_register(void);

/**
 * @brief Notify the connected BLE client with a Rivr binary frame.
 *
 * Sends @p len bytes from @p data using the TX characteristic notify.
 * Silently no-ops if no client is connected or BLE is inactive.
 *
 * Safe to call from the main-loop task only.
 * The frame bytes are copied by the active BLE stack — @p data can be stack-allocated.
 *
 * Increments g_rivr_metrics.ble_tx_frames on success.
 * Increments g_rivr_metrics.ble_errors on notify failure.
 *
 * @param conn_handle  Active BLE connection handle (from rivr_ble_conn_handle()).
 * @param data         Binary Rivr frame bytes.
 * @param len          Frame length in bytes (max RF_MAX_PAYLOAD_LEN).
 */
void rivr_ble_service_notify(uint16_t conn_handle,
                              const uint8_t *data, uint8_t len);

#else  /* RIVR_FEATURE_BLE == 0 */

static inline int  rivr_ble_service_register(void) { return 0; }
static inline void rivr_ble_service_notify(uint16_t c, const uint8_t *d, uint8_t l)
    { (void)c; (void)d; (void)l; }

#endif /* RIVR_FEATURE_BLE */

#ifdef __cplusplus
}
#endif

#endif /* RIVR_BLE_SERVICE_H */
