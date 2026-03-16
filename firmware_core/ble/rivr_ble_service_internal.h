/**
 * @file  rivr_ble_service_internal.h
 * @brief Internal Bluedroid callback surface for the Rivr BLE service.
 */

#ifndef RIVR_BLE_SERVICE_INTERNAL_H
#define RIVR_BLE_SERVICE_INTERNAL_H

#include <stdbool.h>
#include "rivr_config.h"

#if RIVR_FEATURE_BLE

#include "esp_gatts_api.h"

void rivr_ble_service_handle_gatts_event(esp_gatts_cb_event_t event,
                                         esp_gatt_if_t gatts_if,
                                         esp_ble_gatts_cb_param_t *param);
void rivr_ble_service_set_connection(esp_gatt_if_t gatts_if, uint16_t conn_id);
void rivr_ble_service_clear_connection(void);
bool rivr_ble_service_is_ready(void);

#endif

#endif
