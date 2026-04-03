/**
 * @file  nvs_flash.h  (RP2040 compat shim)
 * @brief No-op ESP-IDF NVS flash stub.
 */

#ifndef RIVR_COMPAT_NVS_FLASH_H
#define RIVR_COMPAT_NVS_FLASH_H

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline esp_err_t nvs_flash_init(void) { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_NVS_FLASH_H */
