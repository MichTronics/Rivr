/* esp_task_wdt.h — host-build stub (tests/ only) */
#pragma once

typedef void *TaskHandle_t;

static inline int esp_task_wdt_add(TaskHandle_t handle) { (void)handle; return 0; }
static inline int esp_task_wdt_reset(void)               { return 0; }
static inline int esp_task_wdt_delete(TaskHandle_t h)   { (void)h; return 0; }
