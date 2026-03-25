/**
 * @file  driver/uart.h (Linux compat stub)
 * @brief Maps ESP-IDF UART driver calls to POSIX stdin/stdout for the
 *        Linux serial CLI (terminal input).
 *
 * uart_driver_install(): sets stdin to O_NONBLOCK.
 * uart_read_bytes():     non-blocking read() from STDIN_FILENO.
 * All other functions:   harmless no-ops.
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

/* ── UART port numbers ─────────────────────────────────────────────────── */
#define UART_NUM_0  0
#define UART_NUM_1  1
#define UART_NUM_2  2

/* ── Config constants ─────────────────────────────────────────────────── */
#define UART_DATA_8_BITS           3
#define UART_PARITY_DISABLE        0
#define UART_STOP_BITS_1           1
#define UART_HW_FLOWCTRL_DISABLE   0
#define UART_SCLK_DEFAULT          0

/* ── Config struct ────────────────────────────────────────────────────── */
typedef struct {
    int      baud_rate;
    int      data_bits;
    int      parity;
    int      stop_bits;
    int      flow_ctrl;
    int      rx_flow_ctrl_thresh;
    int      source_clk;
} uart_config_t;

/* ── Stub implementations ─────────────────────────────────────────────── */

static inline esp_err_t uart_param_config(int port, const uart_config_t *cfg)
{
    (void)port; (void)cfg;
    return ESP_OK;
}

/**
 * Set stdin non-blocking so uart_read_bytes() won't block the main loop.
 */
static inline esp_err_t uart_driver_install(int port, int rx_buf, int tx_buf,
                                             int ev_queue_size, void *ev_queue,
                                             int intr_flags)
{
    (void)port; (void)rx_buf; (void)tx_buf;
    (void)ev_queue_size; (void)ev_queue; (void)intr_flags;
    /* Put stdin in non-blocking mode so read() returns immediately. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    return ESP_OK;
}

/** Always reports "installed" so the driver-install block is skippable. */
static inline int uart_is_driver_installed(int port)
{
    (void)port;
    return 1;   /* treat stdin as always ready */
}

/**
 * Non-blocking read of up to @p length bytes from stdin.
 * @p timeout_ms is ignored (stdin is already O_NONBLOCK).
 * Returns number of bytes actually read, or 0 if none available.
 */
static inline int uart_read_bytes(int port, uint8_t *buf, int length,
                                   int timeout_ms)
{
    (void)port; (void)timeout_ms;
    ssize_t r = read(STDIN_FILENO, buf, (size_t)length);
    return (r > 0) ? (int)r : 0;
}
