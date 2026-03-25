/**
 * @file  freertos/task.h (Linux compat stub)
 * @brief Maps vTaskDelay() to nanosleep() for the Linux port.
 */
#pragma once
#include "FreeRTOS.h"
#include <time.h>

static inline void vTaskDelay(TickType_t ticks)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ticks / 1000U),
        .tv_nsec = (long)((ticks % 1000U) * 1000000L),
    };
    nanosleep(&ts, NULL);
}
