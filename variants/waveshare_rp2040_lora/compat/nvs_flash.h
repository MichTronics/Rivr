/**
 * @file  variants/waveshare_rp2040_lora/compat/nvs_flash.h
 * @brief Stub for ESP-IDF nvs_flash.h — no NVS flash partition on RP2040.
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
