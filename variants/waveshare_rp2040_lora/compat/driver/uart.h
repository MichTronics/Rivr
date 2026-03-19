/**
 * @file  variants/waveshare_rp2040_lora/compat/driver/uart.h
 * @brief Stub for ESP-IDF UART driver (RP2040 build).
 *
 * Files like rivr_cli.c and rivr_sources.c include this header for CLI serial
 * I/O on ESP32.  On the Waveshare RP2040 build we provide a thin USB-CDC
 * backend implemented in a C++ translation unit so the existing CLI can keep
 * using the ESP-IDF UART API surface.
 */

#ifndef RIVR_COMPAT_DRIVER_UART_H
#define RIVR_COMPAT_DRIVER_UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_log.h"   /* esp_err_t, ESP_OK */

#ifdef __cplusplus
extern "C" {
#endif

/* ── UART port numbers ───────────────────────────────────────────────────── */
typedef int uart_port_t;
#define UART_NUM_0  ((uart_port_t)0)
#define UART_NUM_1  ((uart_port_t)1)

/* ── UART configuration types ────────────────────────────────────────────── */
typedef enum { UART_DATA_8_BITS = 3 } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;

typedef struct {
    int                  baud_rate;
    uart_word_length_t   data_bits;
    uart_parity_t        parity;
    uart_stop_bits_t     stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t              rx_flow_ctrl_thresh;
    uart_sclk_t          source_clk;
} uart_config_t;

/* ── API implemented by the RP2040 USB-CDC backend ───────────────────────── */
bool uart_is_driver_installed(uart_port_t port);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *cfg);
esp_err_t uart_driver_install(uart_port_t port,
                              int rx_buf,
                              int tx_buf,
                              int queue_size,
                              void *uart_queue,
                              int intr_alloc_flags);
int uart_read_bytes(uart_port_t port,
                    void *buf,
                    uint32_t length,
                    uint32_t ticks_to_wait);
int uart_write_bytes(uart_port_t port, const void *src, size_t size);
esp_err_t uart_driver_delete(uart_port_t port);
esp_err_t uart_flush(uart_port_t port);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);

/* ── Convenience macros expected by ESP-IDF consumers ──────────────────── */
#define UART_PIN_NO_CHANGE  (-1)

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_DRIVER_UART_H */
