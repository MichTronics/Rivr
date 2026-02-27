/* driver/gpio.h — host-build stub (tests/ only, NOT shipped to device) */
#pragma once
#include <stdint.h>

typedef int gpio_num_t;
typedef void (*gpio_isr_t)(void *);

static inline int gpio_isr_handler_add(gpio_num_t gpio,
                                        gpio_isr_t isr, void *arg)
{
    (void)gpio; (void)isr; (void)arg;
    return 0;
}
