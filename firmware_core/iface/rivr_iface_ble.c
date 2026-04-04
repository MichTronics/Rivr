/**
 * @file  rivr_iface_ble.c
 * @brief BLE transport adapter — GATT notify wrapper.
 */

#include "rivr_iface_ble.h"
#include "firmware_core/ble/rivr_ble.h"
#include "firmware_core/ble/rivr_ble_companion.h"
#include "firmware_core/ble/rivr_ble_service.h"

bool rivr_iface_ble_connected(void)
{
    return rivr_ble_is_connected();
}

bool rivr_iface_ble_send(const uint8_t *data, size_t len)
{
    if (!rivr_ble_is_connected()) {
        return false;
    }
    if (!rivr_ble_companion_raw_bridge_enabled()) {
        return false;
    }
    if (!data || len == 0u || len > 255u) {
        return false;
    }
    return rivr_ble_service_notify(rivr_ble_conn_handle(), data, (uint8_t)len);
}
