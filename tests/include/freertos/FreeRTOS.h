/* freertos/FreeRTOS.h — host-build stub (tests/ only) */
#pragma once

typedef unsigned int TickType_t;
#define portNUM_PROCESSORS  1
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFU)

/* ── Spinlock stubs (portMUX / critical-section) ────────────────────────── *
 * On ESP32-IDF these disable interrupts (single-core) or spin+disable
 * (dual-core).  In single-threaded host tests they are no-ops. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED     0
#define portENTER_CRITICAL(mux)          ((void)(mux))
#define portEXIT_CRITICAL(mux)           ((void)(mux))
#define portENTER_CRITICAL_ISR(mux)      ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux)       ((void)(mux))
