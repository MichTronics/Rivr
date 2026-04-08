/**
 * @file  freertos/FreeRTOS.h  (RP2040 compat shim)
 * @brief Minimal FreeRTOS shim for RP2040 Arduino builds without FreeRTOS.
 */

#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

#ifndef pdMS_TO_TICKS
#  define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif

#ifndef portMAX_DELAY
#  define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#endif

#ifndef taskENTER_CRITICAL
#  define taskENTER_CRITICAL() do {} while (0)
#endif

#ifndef taskEXIT_CRITICAL
#  define taskEXIT_CRITICAL() do {} while (0)
#endif
