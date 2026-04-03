/**
 * @file  driver/uart.h  (RP2040 compat shim)
 * @brief Stub for ESP-IDF UART driver against Arduino Serial.
 */

#ifndef RIVR_COMPAT_DRIVER_UART_H
#define RIVR_COMPAT_DRIVER_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

bool rivr_arduino_uart_driver_ready(void);
int rivr_arduino_uart_read(void *buf, uint32_t length);
int rivr_arduino_uart_write(const void *src, size_t size);

typedef int uart_port_t;
#define UART_NUM_0 ((uart_port_t)0)
#define UART_NUM_1 ((uart_port_t)1)

typedef enum { UART_DATA_8_BITS = 3 } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
} uart_config_t;

static inline bool uart_is_driver_installed(uart_port_t port)
{
    (void)port;
    return rivr_arduino_uart_driver_ready();
}

static inline esp_err_t uart_param_config(uart_port_t port, const uart_config_t *cfg)
{
    (void)port; (void)cfg;
    return ESP_OK;
}

static inline esp_err_t uart_driver_install(uart_port_t port,
                                            int rx_buf,
                                            int tx_buf,
                                            int queue_size,
                                            void *uart_queue,
                                            int intr_alloc_flags)
{
    (void)port; (void)rx_buf; (void)tx_buf;
    (void)queue_size; (void)uart_queue; (void)intr_alloc_flags;
    return ESP_OK;
}

static inline int uart_read_bytes(uart_port_t port,
                                  void *buf,
                                  uint32_t length,
                                  uint32_t ticks_to_wait)
{
    (void)port; (void)ticks_to_wait;
    return rivr_arduino_uart_read(buf, length);
}

static inline esp_err_t uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
    (void)port;
    return (esp_err_t)rivr_arduino_uart_write(src, size);
}

static inline esp_err_t uart_driver_delete(uart_port_t port)
{
    (void)port;
    return ESP_OK;
}

static inline esp_err_t uart_flush(uart_port_t port)
{
    (void)port;
    return ESP_OK;
}

static inline esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)port; (void)tx; (void)rx; (void)rts; (void)cts;
    return ESP_OK;
}

#define UART_PIN_NO_CHANGE (-1)

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_DRIVER_UART_H */
