/**
 * @file  firmware_core/platform_rp2040.h
 * @brief RP2040 platform HAL — mirrors the portable platform API used by Rivr.
 *
 * The Waveshare RP2040-LoRa-HF build uses the Earle Philhower Arduino core
 * plus the Pico SDK helpers that ship with the PlatformIO RP2040 platform.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void platform_init(void);

void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len);
void platform_spi_begin(void);
void platform_spi_transfer_raw(const uint8_t *tx, uint8_t *rx, uint16_t len);
void platform_spi_end(void);
void platform_spi_cs_assert(void);
void platform_spi_cs_release(void);

bool platform_sx1262_wait_busy(uint32_t timeout_ms);
void platform_sx1262_reset(void);
void platform_sx1262_set_rxen(bool enable);

void platform_dio1_attach_isr(void (*isr)(void));

void platform_led_on(void);
void platform_led_off(void);
void platform_led_toggle(void);

uint32_t platform_millis(void);
void platform_restart(void);

#ifdef __cplusplus
}
#endif
