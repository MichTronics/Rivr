/**
 * @file  nvs_flash.h  (RP2040 compat shim)
 * @brief ESP-IDF NVS flash init shim backed by LittleFS on RP2040.
 */

#ifndef RIVR_COMPAT_NVS_FLASH_H
#define RIVR_COMPAT_NVS_FLASH_H

#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_NVS_FLASH_H */
