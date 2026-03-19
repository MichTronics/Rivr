/**
 * @file  variants/waveshare_rp2040_lora/compat/freertos/task.h
 * @brief Minimal FreeRTOS task.h stub for RP2040 (arduino-pico, no FreeRTOS).
 *
 * Provides vTaskDelay() mapped to Arduino delay() since there is no RTOS
 * scheduler on the standard arduino-pico build.
 */

#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare Arduino delay() so C-mode TUs can call it without Arduino.h */
void delay(unsigned long ms);

static inline void vTaskDelay(TickType_t ticks_ms)
{
    delay((unsigned long)ticks_ms);
}

static inline void taskYIELD(void) { /* no-op without an RTOS */ }

#ifdef __cplusplus
}
#endif
