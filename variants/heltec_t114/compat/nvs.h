/**
 * @file  nvs.h  (nRF52840 compat shim)
 * @brief Stub for ESP-IDF nvs.h — all NVS operations return ESP_FAIL so the
 *        callers (rivr_embed.c) fall back to compile-time defaults gracefully.
 *
 * The nRF52840 has no ESP-IDF NVS partition.  Persistent identity storage
 * may be added later using LittleFS or similar, but for the initial
 * experimental port the compile-time defaults (RIVR_CALLSIGN, RIVR_NET_ID)
 * are always used.
 */

#ifndef RIVR_COMPAT_NVS_H
#define RIVR_COMPAT_NVS_H

#include <stdint.h>
#include <stddef.h>

/* Import esp_err_t from our esp_log compat; pull in ESP_OK/ESP_FAIL. */
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── NVS handle type ──────────────────────────────────────────────────────── */
typedef uint32_t nvs_handle_t;

/* ── NVS open mode ────────────────────────────────────────────────────────── */
typedef enum {
    NVS_READONLY  = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

/* ── Stub API — all operations "fail" so callers use their defaults ────────── */

static inline esp_err_t nvs_open(const char *ns,
                                  nvs_open_mode_t mode,
                                  nvs_handle_t *out)
{
    (void)ns; (void)mode;
    if (out) *out = 0u;
    return ESP_FAIL; /* no NVS on nRF52 — caller falls back to defaults */
}

static inline esp_err_t nvs_get_str(nvs_handle_t h,
                                     const char   *key,
                                     char         *out,
                                     size_t       *len)
{
    (void)h; (void)key; (void)out; (void)len;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_u16(nvs_handle_t h,
                                     const char   *key,
                                     uint16_t     *out)
{
    (void)h; (void)key; (void)out;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_str(nvs_handle_t h,
                                     const char   *key,
                                     const char   *value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_u16(nvs_handle_t h,
                                     const char   *key,
                                     uint16_t      value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_u32(nvs_handle_t h,
                                     const char   *key,
                                     uint32_t      value)
{
    (void)h; (void)key; (void)value;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_u32(nvs_handle_t h,
                                     const char   *key,
                                     uint32_t     *out)
{
    (void)h; (void)key; (void)out;
    return ESP_FAIL;
}

static inline esp_err_t nvs_get_blob(nvs_handle_t h,
                                      const char   *key,
                                      void         *out,
                                      size_t       *len)
{
    (void)h; (void)key; (void)out; (void)len;
    return ESP_FAIL;
}

static inline esp_err_t nvs_set_blob(nvs_handle_t h,
                                      const char   *key,
                                      const void   *value,
                                      size_t        len)
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
