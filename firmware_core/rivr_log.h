#pragma once
#include <stdint.h>
#include "esp_log.h"

typedef enum {
    RIVR_LOG_DEBUG   = 0,  /* full ESP_LOG + @MET metrics          */
    RIVR_LOG_METRICS = 1,  /* @MET metrics only (no info logs)     */
    RIVR_LOG_SILENT  = 2   /* no output except warnings + errors   */
} rivr_log_mode_t;

extern rivr_log_mode_t g_rivr_log_mode;

void            rivr_log_set_mode(rivr_log_mode_t mode);
rivr_log_mode_t rivr_log_get_mode(void);

/* Informational – suppressed in METRICS and SILENT modes */
#define RIVR_LOGI(tag, fmt, ...) \
    do { \
        if (g_rivr_log_mode == RIVR_LOG_DEBUG) { \
            ESP_LOGI(tag, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/* Warnings – always visible */
#define RIVR_LOGW(tag, fmt, ...)  ESP_LOGW(tag, fmt, ##__VA_ARGS__)

/* Errors – always visible */
#define RIVR_LOGE(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)
