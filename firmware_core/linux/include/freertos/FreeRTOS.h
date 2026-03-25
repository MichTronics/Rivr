/**
 * @file  freertos/FreeRTOS.h (Linux compat stub)
 * @brief Minimal FreeRTOS type and constant shim for the Linux port.
 *
 * Only provides what the Rivr firmware_core and rivr_layer files actually
 * reference.  No actual RTOS is needed on Linux — the main loop runs as a
 * single pthread.
 */
#pragma once
#include <stdint.h>

typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;

#define portMAX_DELAY         ((TickType_t)0xFFFFFFFFUL)
#define portTICK_PERIOD_MS    1U

/** Convert milliseconds to ticks (1 tick = 1 ms in this stub). */
#define pdMS_TO_TICKS(ms)     ((TickType_t)(ms))

/* Dummy critical-section macros (single-threaded main loop on Linux). */
#define taskENTER_CRITICAL()  do {} while (0)
#define taskEXIT_CRITICAL()   do {} while (0)
