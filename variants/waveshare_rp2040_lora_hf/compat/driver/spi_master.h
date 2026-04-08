/**
 * @file  driver/spi_master.h  (RP2040 compat shim)
 * @brief Stub for ESP-IDF SPI master driver types.
 */

#ifndef RIVR_COMPAT_DRIVER_SPI_MASTER_H
#define RIVR_COMPAT_DRIVER_SPI_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *spi_device_handle_t;

typedef struct {
    uint8_t cmd;
    uint32_t addr;
    size_t length;
    uint8_t flags;
    void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;

static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    (void)h; (void)t;
    return ESP_OK;
}

static inline esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    (void)h; (void)t;
    return ESP_OK;
}

static inline esp_err_t spi_device_acquire_bus(spi_device_handle_t h, uint32_t wait)
{
    (void)h; (void)wait;
    return ESP_OK;
}

static inline void spi_device_release_bus(spi_device_handle_t h)
{
    (void)h;
}

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_DRIVER_SPI_MASTER_H */
