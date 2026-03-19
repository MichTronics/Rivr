#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {

typedef int uart_port_t;

typedef struct {
    int      baud_rate;
    int      data_bits;
    int      parity;
    int      stop_bits;
    int      flow_ctrl;
    uint8_t  rx_flow_ctrl_thresh;
    int      source_clk;
} uart_config_t;

typedef int esp_err_t;

#ifndef UART_NUM_0
#define UART_NUM_0  ((uart_port_t)0)
#endif

#ifndef ESP_OK
#define ESP_OK   0
#define ESP_FAIL (-1)
#endif
}

namespace {

static bool s_uart0_driver_installed = false;

static bool uart_port_supported(uart_port_t port)
{
    return port == UART_NUM_0;
}

}  // namespace

extern "C" bool uart_is_driver_installed(uart_port_t port)
{
    return uart_port_supported(port) && s_uart0_driver_installed;
}

extern "C" esp_err_t uart_param_config(uart_port_t port, const uart_config_t *cfg)
{
    if (!uart_port_supported(port)) {
        return ESP_FAIL;
    }

    unsigned long baud = 115200UL;
    if (cfg != nullptr && cfg->baud_rate > 0) {
        baud = (unsigned long)cfg->baud_rate;
    }

    Serial.begin(baud);
    return ESP_OK;
}

extern "C" esp_err_t uart_driver_install(uart_port_t port,
                                         int rx_buf,
                                         int tx_buf,
                                         int queue_size,
                                         void *uart_queue,
                                         int intr_alloc_flags)
{
    (void)rx_buf;
    (void)tx_buf;
    (void)queue_size;
    (void)uart_queue;
    (void)intr_alloc_flags;

    if (!uart_port_supported(port)) {
        return ESP_FAIL;
    }

    s_uart0_driver_installed = true;
    return ESP_OK;
}

extern "C" int uart_read_bytes(uart_port_t port,
                               void *buf,
                               uint32_t length,
                               uint32_t ticks_to_wait)
{
    (void)ticks_to_wait;

    if (!uart_port_supported(port) || buf == nullptr || length == 0u) {
        return 0;
    }

    uint8_t *out = static_cast<uint8_t *>(buf);
    uint32_t read = 0u;

    while (read < length && Serial.available() > 0) {
        int ch = Serial.read();
        if (ch < 0) {
            break;
        }
        out[read++] = (uint8_t)ch;
    }

    return (int)read;
}

extern "C" int uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
    if (!uart_port_supported(port) || src == nullptr) {
        return -1;
    }

    return (int)Serial.write(static_cast<const uint8_t *>(src), size);
}

extern "C" esp_err_t uart_driver_delete(uart_port_t port)
{
    if (!uart_port_supported(port)) {
        return ESP_FAIL;
    }

    s_uart0_driver_installed = false;
    return ESP_OK;
}

extern "C" esp_err_t uart_flush(uart_port_t port)
{
    if (!uart_port_supported(port)) {
        return ESP_FAIL;
    }

    Serial.flush();
    return ESP_OK;
}

extern "C" esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)tx;
    (void)rx;
    (void)rts;
    (void)cts;

    if (!uart_port_supported(port)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
