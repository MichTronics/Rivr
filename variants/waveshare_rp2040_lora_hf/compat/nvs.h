/**
 * @file  nvs.h  (RP2040 compat shim)
 * @brief ESP-IDF-style NVS API backed by LittleFS on RP2040 builds.
 */

#ifndef RIVR_COMPAT_NVS_H
#define RIVR_COMPAT_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ESP_ERR_NOT_FOUND
#  define ESP_ERR_NOT_FOUND 0x105
#endif

typedef int32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_NVS_H */
