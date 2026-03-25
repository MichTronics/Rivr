/**
 * @file  platform_linux.h
 * @brief Raspberry Pi Linux platform HAL — SPI and GPIO pin definitions.
 *
 * Wiring of the E22-900M30S (SX1262) to a Raspberry Pi via the 40-pin header:
 *
 *   E22 pin | Signal | RPi GPIO (BCM) | Description
 *   --------|--------|----------------|-------------------------------
 *     6     | RXEN   | GPIO12         | RF switch: RX enable (active high)
 *     7     | TXEN   | GPIO13         | RF switch: TX enable (active high)
 *    13     | DIO1   | GPIO16         | IRQ: TxDone | RxDone | Timeout
 *    14     | BUSY   | GPIO20         | Chip busy flag (active high)
 *    15     | NRST   | GPIO18         | Reset (active low)
 *    16     | MISO   | GPIO9          | SPI0 MISO
 *    17     | MOSI   | GPIO10         | SPI0 MOSI
 *    18     | SCK    | GPIO11         | SPI0 SCLK
 *    19     | NSS    | GPIO21         | Chip-select (manual, active low)
 *
 *  SPI device: /dev/spidev0.0  (SPI0, CE0 — CS unused by kernel, handled manually)
 *  GPIO chip:  /dev/gpiochip0  (BCM GPIO, standard on all Raspberry Pi models)
 *
 * This header mirrors the interface of platform_esp32.h so the rest of the
 * firmware links against an identical API regardless of platform.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── SX1262 SPI / control GPIO assignments (BCM numbers) ─────────────────── */
#define PIN_SX1262_RXEN    12   /**< RF switch RX enable                      */
#define PIN_SX1262_TXEN    13   /**< RF switch TX enable                      */
#define PIN_SX1262_DIO1    16   /**< IRQ: TxDone | RxDone | Timeout           */
#define PIN_SX1262_BUSY    20   /**< Chip BUSY flag (active high)              */
#define PIN_SX1262_RESET   18   /**< NRST – active low                        */
#define PIN_SX1262_NSS     21   /**< Manual chip-select (active low)           */

/* SPI bus configuration */
#define RIVR_LINUX_SPI_DEV  "/dev/spidev0.0"
#define SX1262_SPI_FREQ_HZ  8000000UL   /**< 8 MHz — SX1262 max is 16 MHz     */

/* GPIO chip (BCM GPIO on all RPi models) */
#define RIVR_LINUX_GPIO_CHIP  "/dev/gpiochip0"

/* UART CLI baud — only used for the UART_CLI_BAUD macro referenced from
 * platform_esp32.h when that header is force-included on Linux builds. */
#define UART_CLI_BAUD       115200UL

/* RF switch enabled on the E22-900M30S (has external PA + RF switch). */
#define RIVR_RFSWITCH_ENABLE  1

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialise all Linux platform hardware.
 *
 * Opens the spidev SPI bus, configures all GPIO lines via libgpiod, starts
 * the DIO1 interrupt monitoring thread, and sets stdin to O_NONBLOCK for the
 * serial CLI.  Must be called once from main() before any other subsystem.
 */
void platform_init(void);

/* ── SPI helpers (same interface as platform_esp32.h) ────────────────────── */

/** SPI full-duplex transfer to SX1262. */
void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len);

/** Assert (drive low) the SX1262 chip-select line (GPIO21). */
void platform_spi_cs_assert(void);

/** Release (drive high) the SX1262 chip-select line (GPIO21). */
void platform_spi_cs_release(void);

/** Wait until SX1262 BUSY (GPIO20) goes low.  Returns false on timeout. */
bool platform_sx1262_wait_busy(uint32_t timeout_ms);

/** Hardware-reset the SX1262 (pulse NRST GPIO18 low for 1 ms). */
void platform_sx1262_reset(void);

/** Drive RF-switch RXEN (GPIO12) / TXEN (GPIO13).
 *  enable=true  → RX path (RXEN=1, TXEN=0)
 *  enable=false → TX path (RXEN=0, TXEN=1) */
void platform_sx1262_set_rxen(bool enable);

/* ── DIO1 interrupt ──────────────────────────────────────────────────────── */

/**
 * @brief Register a no-arg callback fired on every DIO1 rising edge.
 *
 * Internally starts a pthread that blocks on gpiod_line_event_wait() so the
 * main thread is never blocked.  The callback must be very short (set a flag).
 */
void platform_dio1_attach_isr(void (*isr)(void));

/* ── LED (no onboard LED stub — no-ops) ─────────────────────────────────── */
void platform_led_on(void);
void platform_led_off(void);
void platform_led_toggle(void);

/* ── Delay ───────────────────────────────────────────────────────────────── */
void platform_delay_ms(uint32_t ms);

/* ── Misc ─────────────────────────────────────────────────────────────────── */
void     platform_restart(void);
uint32_t platform_millis(void);

#ifdef __cplusplus
}
#endif
