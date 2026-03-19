/**
 * @file  variants/waveshare_rp2040_lora/compat/freertos/FreeRTOS.h
 * @brief Minimal FreeRTOS stub for RP2040 (arduino-pico, no FreeRTOS).
 *
 * arduino-pico does not ship FreeRTOS.  Shared Rivr source files
 * (rivr_embed.c, rivr_cli.c) include "freertos/FreeRTOS.h" and
 * "freertos/task.h" for vTaskDelay() and pdMS_TO_TICKS().
 *
 * This stub provides only the narrow subset actually used; the RP2040
 * platform files (timebase, radio, main) do NOT use FreeRTOS at all.
 */

#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef void *   TaskHandle_t;
typedef void *   TimerHandle_t;
typedef void *   QueueHandle_t;
typedef int      BaseType_t;

#define portTICK_PERIOD_MS   1u
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))
#define portMAX_DELAY        ((TickType_t)0xFFFFFFFFUL)
#define pdTRUE               ((BaseType_t)1)
#define pdFALSE              ((BaseType_t)0)
#define pdPASS               pdTRUE
#define pdFAIL               pdFALSE
