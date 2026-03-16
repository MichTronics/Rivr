/**
 * @file  rivr_iface_ble.h
 * @brief BLE transport adapter for the Rivr packet bus.
 *
 * Wraps BLE GATT notify so the bus can push frames to a connected
 * BLE client without knowing stack-specific connection-handle mechanics.
 *
 * All functions compile to inline no-ops when RIVR_FEATURE_BLE=0 via the
 * stubs in rivr_ble.h and rivr_ble_service.h.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Test whether a BLE client is currently connected.
 * @return true if BLE has an active connection
 */
bool rivr_iface_ble_connected(void);

/**
 * @brief Send a frame to the connected BLE client via GATT notify.
 *
 * No-op (returns false) if no client is connected or RIVR_FEATURE_BLE=0.
 *
 * @param data  Encoded Rivr frame bytes
 * @param len   Frame length (1–255 bytes)
 * @return true  if the notify was submitted successfully
 * @return false if not connected or the BLE stack returned an error
 */
bool rivr_iface_ble_send(const uint8_t *data, size_t len);
