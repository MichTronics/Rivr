/**
 * @file  driver/spi_master.h (Linux compat stub)
 * @brief Type stub — only the spi_device_handle_t type and SPI host
 *        constants are needed by platform_esp32.h; actual SPI is handled
 *        by platform_linux.c via the Linux spidev kernel interface.
 */
#pragma once
#include <stdint.h>

/* Opaque handle type — a plain int is sufficient as a stub. */
typedef int spi_device_handle_t;

/* SPI host device constants (mirroring ESP-IDF spi_host_device_t). */
typedef enum {
    SPI1_HOST = 0,
    SPI2_HOST = 1,   /**< = HSPI */
    SPI3_HOST = 2,   /**< = VSPI */
} spi_host_device_t;

#define SPI_DMA_CH_AUTO 0
