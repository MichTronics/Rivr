/**
 * @file  freertos/task.h  (RP2040 compat shim)
 * @brief Maps `vTaskDelay()` to Pico SDK sleep plus timebase sync.
 */

#pragma once

#include "FreeRTOS.h"
#include "firmware_core/timebase.h"
 
#ifdef __cplusplus
extern "C" {
#endif
void sleep_ms(uint32_t ms);
#ifdef __cplusplus
}
#endif

static inline void vTaskDelay(TickType_t ticks)
{
    sleep_ms(ticks ? ticks : 1u);
    timebase_tick_hook();
}
