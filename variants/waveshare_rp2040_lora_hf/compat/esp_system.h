/**
 * @file  esp_system.h  (RP2040 compat shim)
 * @brief Minimal ESP-IDF system stub for RP2040.
 */

#ifndef RIVR_COMPAT_ESP_SYSTEM_H
#define RIVR_COMPAT_ESP_SYSTEM_H

#include <stdint.h>
#include <hardware/watchdog.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline __attribute__((noreturn)) void esp_restart(void)
{
    watchdog_reboot(0, 0, 0);
    while (1) {}
}

static inline const char *esp_get_idf_version(void) { return "RP2040-Arduino"; }

typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON = 1,
    ESP_RST_SW = 3,
    ESP_RST_PANIC = 4,
    ESP_RST_WDT = 6,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_UNKNOWN; }

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_ESP_SYSTEM_H */
