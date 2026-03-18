/**
 * @file  driver/spi_master.h  (nRF52840 compat shim)
 * @brief Stub for ESP-IDF SPI master driver types.
 *
 * platform_esp32.h is included via relative path by rivr_sources.c and
 * other shared files.  It in turn includes "driver/spi_master.h" for the
 * spi_device_handle_t type and extern g_spi_sx1262 declaration.
 * On nRF52840, SPI is handled by platform_nrf52.c using the Arduino SPI
 * library; this stub satisfies the type declarations without pulling in
 * any ESP-IDF code.
 */

#ifndef RIVR_COMPAT_DRIVER_SPI_MASTER_H
#define RIVR_COMPAT_DRIVER_SPI_MASTER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"   /* esp_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle type — on ESP32 this is a pointer; stub as void* */
typedef void *spi_device_handle_t;

/* Minimal transaction descriptor (fields used by shared headers only) */
typedef struct {
    uint8_t  cmd;
    uint32_t addr;
    size_t   length;   /* bits */
    uint8_t  flags;
    void    *tx_buffer;
    void    *rx_buffer;
} spi_transaction_t;

/* Stub functions — never called on nRF52 (platform_esp32.c is excluded) */
static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t h,
                                                      spi_transaction_t  *t)
{
    (void)h; (void)t;
    return ESP_OK;
}

static inline esp_err_t spi_device_transmit(spi_device_handle_t h,
                                              spi_transaction_t  *t)
{
    (void)h; (void)t;
    return ESP_OK;
}

static inline esp_err_t spi_device_acquire_bus(spi_device_handle_t h, uint32_t wait)
{
    (void)h; (void)wait;
    return ESP_OK;
}

static inline void spi_device_release_bus(spi_device_handle_t h) { (void)h; }

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_DRIVER_SPI_MASTER_H */
