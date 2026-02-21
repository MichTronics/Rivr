/**
 * @file  platform_esp32.h
 * @brief ESP32-specific GPIO/SPI pin assignments and hardware initialisation.
 *
 * All pin numbers are for ESP32-DevKitC-V4 + HelTec LoRa32 variant.
 * Override by editing this header or via -DPIN_xxx=N in your platformio.ini.
 *
 * SX1262 SPI wiring (4-wire + 5 control pins):
 *   SCK  = 18  (VSPI_CLK)
 *   MOSI = 23  (VSPI_MOSI)
 *   MISO = 19  (VSPI_MISO)
 *   NSS  =  5  (chip select, active low)
 *   BUSY = 32  (SX1262 busy flag)
 *   RESET= 25  (NRST – active low)
 *   DIO1 = 33  (IRQ: TxDone | RxDone | Timeout)
 *   RXEN = 14  (RX/TX antenna switch – high = receive)
 *   TXEN = 13  (RX/TX antenna switch – high = transmit, driven by DIO2)
 */

#ifndef PLATFORM_ESP32_H
#define PLATFORM_ESP32_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── SX1262 SPI / control pin assignments ────────────────────────────────── */
#ifndef SX1262_SPI_HOST
#  define SX1262_SPI_HOST   SPI2_HOST     /* VSPI */
#endif
#ifndef SX1262_SPI_FREQ_HZ
#  define SX1262_SPI_FREQ_HZ  8000000UL   /* 8 MHz */
#endif

#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK    18
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI   23
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO   19
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS     5
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY   32
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET  25
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1   33
#endif
/** RXEN: drive HIGH to enable the receive path of the RF switch. */
#ifndef PIN_SX1262_RXEN
#  define PIN_SX1262_RXEN   14
#endif
/** TXEN: driven automatically by SX1262 DIO2 (SetDio2AsRfSwitchCtrl).
 *  Also routed to ESP32 GPIO13 for initialisation / override if needed. */
#ifndef PIN_SX1262_TXEN
#  define PIN_SX1262_TXEN   13
#endif

/* ── Status LED ──────────────────────────────────────────────────────────── */
#ifndef PIN_LED_STATUS
#  define PIN_LED_STATUS     2   /* GPIO2 = onboard LED on ESP32-DevKit */
#endif

/* ── UART / USB serial ───────────────────────────────────────────────────── */
#ifndef UART_CLI_BAUD
#  define UART_CLI_BAUD     115200UL
#endif

/* ── SPI handle (exported after platform_init) ───────────────────────────── */
#include "driver/spi_master.h"
extern spi_device_handle_t g_spi_sx1262;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialise all platform hardware.
 *
 * Call once from app_main() BEFORE any other subsystem init.
 * Performs:
 *   1. NVS / flash initialisation
 *   2. GPIO output + input configuration
 *   3. VSPI bus initialisation
 *   4. SX1262 SPI device registration
 *   5. DIO1 GPIO interrupt allocation (radio_isr attached separately)
 *   6. Status LED initial state
 */
void platform_init(void);

/* ── SPI helpers (thin wrappers keeping radio driver portable) ───────────── */

/**
 * @brief SPI full-duplex transfer to SX1262.
 *
 * Asserts NSS, clocks `len` bytes out of `tx` while simultaneously
 * reading `len` bytes into `rx`. Releases NSS when done.
 * Blocks until transfer complete (DMA + semaphore wait).
 *
 * Call ONLY from main loop or radio task, NEVER from ISR.
 */
void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len);

/** Assert (drive low) the SX1262 chip-select line. */
void platform_spi_cs_assert(void);

/** Release (drive high) the SX1262 chip-select line. */
void platform_spi_cs_release(void);

/** Wait until SX1262 BUSY pin goes low (with timeout_ms). Returns false on timeout. */
bool platform_sx1262_wait_busy(uint32_t timeout_ms);

/** Hard-reset the SX1262 (toggle RESET low for 1 ms). */
void platform_sx1262_reset(void);

/* ── Antenna-switch helpers ─────────────────────────────────────────────── */
/**
 * Drive RXEN pin.  Call with true before SetRx, false before SetTx.
 * TXEN is driven automatically by the SX1262 via DIO2 (SetDio2AsRfSwitchCtrl);
 * this GPIO is set low at boot and left to hardware during normal operation.
 */
void platform_sx1262_set_rxen(bool enable);

/* ── LED helpers ─────────────────────────────────────────────────────────── */
void platform_led_on(void);
void platform_led_off(void);
void platform_led_toggle(void);

/* ── Delay ───────────────────────────────────────────────────────────────── */
/** Busy-wait for at least `ms` milliseconds. Uses FreeRTOS vTaskDelay. */
void platform_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_ESP32_H */
