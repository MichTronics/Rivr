/* esp_log.h — host-build stub (tests/ only, NOT shipped to device)
 * Redirects ESP_LOG* macros to fprintf so log output is visible on stdout
 * and not silently swallowed during testing. */
#pragma once
#include <stdio.h>

#define ESP_LOGI(tag, fmt, ...)  fprintf(stdout, "[I][%-8s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)  fprintf(stderr, "[W][%-8s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...)  fprintf(stderr, "[E][%-8s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  /* debug suppressed in tests */

/* Silence unused-parameter warnings from the tag argument */
static inline const char *_esp_log_tag_nop(const char *t) { return t; }
