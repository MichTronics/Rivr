/**
 * @file  nvs.h (Linux compat stub)
 * @brief NVS key-value store stub — always returns ESP_ERR_NOT_FOUND so
 *        the firmware falls through to the compiled-in default RIVR program.
 */
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef int nvs_handle_t;
typedef int nvs_open_mode_t;

#define NVS_READONLY   0
#define NVS_READWRITE  1

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode,
                                  nvs_handle_t *out_handle)
{
    (void)ns; (void)mode;
    *out_handle = 0;
    return ESP_ERR_NOT_FOUND;   /* triggers default-program fallback */
}

static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *key,
                                     char *out_value, size_t *length)
{
    (void)h; (void)key; (void)out_value; (void)length;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *key,
                                     const char *value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_u16(nvs_handle_t h, const char *key,
                                     uint16_t *out_value)
{
    (void)h; (void)key; (void)out_value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_u16(nvs_handle_t h, const char *key,
                                     uint16_t value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_u32(nvs_handle_t h, const char *key,
                                     uint32_t *out_value)
{
    (void)h; (void)key; (void)out_value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_u32(nvs_handle_t h, const char *key,
                                     uint32_t value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_i32(nvs_handle_t h, const char *key,
                                     int32_t *out_value)
{
    (void)h; (void)key; (void)out_value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_i32(nvs_handle_t h, const char *key,
                                     int32_t value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h; (void)key;
    return ESP_OK;
}

static inline esp_err_t nvs_commit(nvs_handle_t h)  { (void)h; return ESP_OK; }
static inline void      nvs_close(nvs_handle_t h)   { (void)h; }
