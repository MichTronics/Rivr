/**
 * @file  variants/heltec_t114/compat/esp_log.h
 * @brief Shim replacing ESP-IDF esp_log.h for nRF52840 Arduino builds.
 *
 * Shared source files (rivr_fabric.c, dutycycle.c, rivr_log.h …) include
 * "esp_log.h" directly.  This compat header provides the same macros so
 * those files compile unchanged on nRF52.
 *
 * Output goes to Arduino Serial (initialized in main_nrf52.cpp).
 * The format is kept identical to ESP-IDF's output for tooling compatibility.
 */

#pragma once

#ifdef __cplusplus
#include <Arduino.h>
static inline void _rivr_serial_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}
#define _LOG_PRINT _rivr_serial_printf
#else
#include <stdio.h>
#define _LOG_PRINT printf
#endif

#define ESP_LOGI(tag, fmt, ...) _LOG_PRINT("[I][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) _LOG_PRINT("[D][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) _LOG_PRINT("[W][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) _LOG_PRINT("[E][%s] " fmt "\r\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) /* verbose suppressed */

/* esp_err_t — used by some ESP-IDF macros */
#ifndef ESP_OK
#  define ESP_OK   0
#  define ESP_FAIL (-1)
typedef int esp_err_t;
#endif

/* ESP_ERROR_CHECK — on nRF52 just assert */
#ifndef ESP_ERROR_CHECK
#  define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) { \
        _LOG_PRINT("[E][ESP_ERR] %s:%d: error %d\r\n", __FILE__, __LINE__, (int)(x)); \
        while(1); } } while(0)
#endif
