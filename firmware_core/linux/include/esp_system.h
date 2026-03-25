/**
 * @file  esp_system.h (Linux compat stub)
 * @brief Minimal ESP-IDF system API shim — maps esp_restart() to exit().
 */
#pragma once
#include <stdlib.h>

static inline void esp_restart(void) { exit(0); }
