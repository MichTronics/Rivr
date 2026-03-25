/**
 * @file  esp_log.h (Linux compat stub)
 * @brief printf-based replacement for ESP-IDF logging macros.
 *
 * Maps ESP_LOGx(tag, fmt, ...) → timestamped printf to stdout/stderr.
 * Also provides the esp_get_idf_version() stub.
 */
#pragma once
#include <stdio.h>
#include <time.h>

static inline unsigned long _esp_log_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}

#define ESP_LOGI(tag, fmt, ...) \
    printf("[I][%lums][%s] " fmt "\n", _esp_log_ms(), (tag), ##__VA_ARGS__)

#define ESP_LOGD(tag, fmt, ...) \
    printf("[D][%lums][%s] " fmt "\n", _esp_log_ms(), (tag), ##__VA_ARGS__)

#define ESP_LOGW(tag, fmt, ...) \
    printf("[W][%lums][%s] " fmt "\n", _esp_log_ms(), (tag), ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) \
    fprintf(stderr, "[E][%lums][%s] " fmt "\n", _esp_log_ms(), (tag), ##__VA_ARGS__)

static inline const char *esp_get_idf_version(void) { return "linux-port-0.1"; }
