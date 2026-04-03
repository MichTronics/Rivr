/**
 * @file  nvs.h  (RP2040 compat shim)
 * @brief Stub for ESP-IDF NVS — callers fall back to compile-time defaults.
 */

#ifndef RIVR_COMPAT_NVS_H
#define RIVR_COMPAT_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)ns;
    (void)mode;
    if (out) {
        *out = 0u;
    }
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h; (void)key; (void)out; (void)len;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    (void)h; (void)key; (void)out;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h; (void)key; (void)out;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h; (void)key; (void)out; (void)len;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    (void)h; (void)key; (void)value; (void)len;
    return ESP_FAIL;
}

static inline esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_FAIL;
}

static inline void nvs_close(nvs_handle_t h)
{
    (void)h;
}

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_NVS_H */
