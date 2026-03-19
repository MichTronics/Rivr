/**
 * @file  firmware_core/platform_rp2040.h
 * @brief RP2040 platform HAL — mirrors the interface of platform_nrf52.h.
 *
 * Provides the same function signatures so radio_sx1262_rp2040.c and other
 * shared drivers call identical platform_* functions regardless of the MCU.
 *
 * SX1262 pin numbers are defined in variants/waveshare_rp2040_lora/config.h
 * and injected via -include at compile time.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialise platform hardware (GPIO, SPI1, LED).
 * Call once from setup() before any other subsystem.
 */
void platform_init(void);

/* ── SPI helpers ─────────────────────────────────────────────────────────── */

/** SPI full-duplex transfer to SX1262: beginTransaction + CS + bytes + CS + endTransaction. */
void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len);

/**
 * Begin a multi-chunk SPI transaction (beginTransaction + CS low).
 * Must be followed by one or more platform_spi_write_read_raw() calls
 * and then platform_spi_end().
 */
void platform_spi_begin(void);

/** End a multi-chunk SPI transaction (CS high + endTransaction). */
void platform_spi_end(void);

/**
 * Raw SPI byte transfer — NO CS or transaction management.
 * Must be called between platform_spi_begin() and platform_spi_end().
 * tx may be NULL (sends 0x00); rx may be NULL (discards received bytes).
 */
void platform_spi_write_read_raw(const uint8_t *tx, uint8_t *rx, uint16_t len);

/** Assert (drive low) the SX1262 chip-select line. */
void platform_spi_cs_assert(void);

/** Release (drive high) the SX1262 chip-select line. */
void platform_spi_cs_release(void);

/** Wait until SX1262 BUSY pin goes low.  Returns false on timeout. */
bool platform_sx1262_wait_busy(uint32_t timeout_ms);

/** Hardware-reset the SX1262 (pulse RESET low for 1 ms then release). */
void platform_sx1262_reset(void);

/** Drive RF-switch control lines.  No-op when RIVR_RFSWITCH_ENABLE=0. */
void platform_sx1262_set_rxen(bool enable);

/* ── GPIO ISR ────────────────────────────────────────────────────────────── */

/**
 * @brief Attach a rising-edge interrupt handler to PIN_SX1262_DIO1.
 *
 * The handler is called from the Arduino interrupt context so it must be
 * kept very short (set a flag, no SPI).
 */
void platform_dio1_attach_isr(void (*isr)(void));

/* ── LED ─────────────────────────────────────────────────────────────────── */
void platform_led_on(void);
void platform_led_off(void);
void platform_led_toggle(void);

/* ── Misc ────────────────────────────────────────────────────────────────── */

/** Milliseconds since boot (wraps ~49 days).  Safe from any context. */
uint32_t platform_millis(void);

/** Reboot the device. */
void platform_restart(void);

#ifdef __cplusplus
}
#endif
