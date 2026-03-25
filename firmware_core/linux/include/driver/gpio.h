/**
 * @file  driver/gpio.h (Linux compat stub)
 * @brief Type stub — GPIO constants from platform_esp32.h; actual GPIO
 *        control is handled by platform_linux.c via libgpiod.
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef uint32_t gpio_num_t;
typedef uint32_t gpio_mode_t;
typedef uint32_t gpio_pullup_t;
typedef uint32_t gpio_pulldown_t;
typedef uint32_t gpio_int_type_t;

#define GPIO_MODE_OUTPUT          1U
#define GPIO_MODE_INPUT           0U
#define GPIO_PULLUP_DISABLE       0U
#define GPIO_PULLDOWN_DISABLE     0U
#define GPIO_INTR_DISABLE         0U
#define GPIO_INTR_POSEDGE         1U

typedef struct {
    uint64_t         pin_bit_mask;
    gpio_mode_t      mode;
    gpio_pullup_t    pull_up_en;
    gpio_pulldown_t  pull_down_en;
    gpio_int_type_t  intr_type;
} gpio_config_t;

/* Stub implementations — no-ops; real GPIO via platform_linux.c/libgpiod. */
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return 0; }
static inline esp_err_t gpio_set_level(gpio_num_t n, uint32_t v) { (void)n; (void)v; return 0; }
static inline int       gpio_get_level(gpio_num_t n) { (void)n; return 0; }
static inline esp_err_t gpio_install_isr_service(int f) { (void)f; return 0; }
static inline esp_err_t gpio_isr_handler_add(gpio_num_t n, void (*h)(void*), void *a)
    { (void)n; (void)h; (void)a; return 0; }
