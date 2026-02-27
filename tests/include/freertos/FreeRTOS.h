/* freertos/FreeRTOS.h — host-build stub (tests/ only) */
#pragma once

typedef unsigned int TickType_t;
#define portNUM_PROCESSORS  1
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFU)
