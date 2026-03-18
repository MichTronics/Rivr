/**
 * @file  nvs_flash.h  (nRF52840 compat shim)
 * @brief Stub for ESP-IDF nvs_flash.h — no NVS flash partition on nRF52.
 *
 * nvs_flash_init() is typically called during platform init on ESP32.
 * On nRF52 platform_init() does not call it, and the linker would fail if
 * any compiled TU referenced the real symbol.  This stub provides a no-op
 * definition that returns ESP_OK.
 */

#ifndef RIVR_COMPAT_NVS_FLASH_H
#define RIVR_COMPAT_NVS_FLASH_H

#include "esp_log.h"   /* for esp_err_t, ESP_OK */

#ifdef __cplusplus
extern "C" {
#endif

static inline esp_err_t nvs_flash_init(void) { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_NVS_FLASH_H */
