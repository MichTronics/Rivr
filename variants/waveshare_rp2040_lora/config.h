/**
 * @file  variants/waveshare_rp2040_lora/config.h
 * @brief Pin assignments for the Waveshare RP2040 LoRa (RP2040 + SX1262).
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  Waveshare RP2040 LoRa                                                  │
 * │  MCU : RP2040 (ARM Cortex-M0+, 2 MB flash, 256 KB SRAM)                │
 * │  Radio: Semtech SX1262 (on-board, connected to SPI1)                    │
 * │  USB  : Native USB (RP2040 USB controller) via USB CDC-ACM              │
 * │                                                                         │
 * │  Pin mapping from framework-arduinopico board definition:               │
 * │  ~/.platformio/packages/framework-arduinopico/variants/                 │
 * │    waveshare_rp2040_lora/pins_arduino.h                                 │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * SX1262 SPI wiring (RP2040 GPIO, SPI1 bus):
 *   CS    = GPIO13   (software chip select, active low)
 *   SCK   = GPIO14   (SPI1 SCK)
 *   MOSI  = GPIO15   (SPI1 TX)
 *   DIO1  = GPIO16   (IRQ: TxDone | RxDone | Timeout)
 *   ANT   = GPIO17   (external RF switch: HIGH=RX/idle, LOW=TX)
 *   BUSY  = GPIO18   (SX1262 busy flag, active high)
 *   RESET = GPIO23   (NRST, active low)
 *   MISO  = GPIO24   (SPI1 RX)
 *   LED   = GPIO25   (onboard LED, active HIGH)
 */

#pragma once

/* ── SX1262 SPI / control pins ────────────────────────────────────────── */
#define PIN_SX1262_NSS      13   /* GPIO13 – chip select (software)  */
#define PIN_SX1262_SCK      14   /* GPIO14 – SPI1 SCK                */
#define PIN_SX1262_MOSI     15   /* GPIO15 – SPI1 TX                 */
#define PIN_SX1262_DIO1     16   /* GPIO16 – IRQ line                */
#define PIN_SX1262_BUSY     18   /* GPIO18 – busy flag               */
#define PIN_SX1262_RESET    23   /* GPIO23 – hardware reset          */
#define PIN_SX1262_MISO     24   /* GPIO24 – SPI1 RX                 */

/* Waveshare routes the antenna switch to RP2040 GPIO17, not SX1262 DIO2.
 * Per the vendor docs: keep it HIGH for RX/idle and LOW while transmitting.
 */
#define PIN_SX1262_ANT_SW          17
#define RIVR_SX1262_USE_DIO2_RF_SWITCH  0
#define RIVR_RFSWITCH_ENABLE       0

/* ── Status LED ───────────────────────────────────────────────────────── */
#define PIN_LED_STATUS      25   /* GPIO25 – onboard LED (active HIGH) */

/* ── Radio driver selection ───────────────────────────────────────────── */
#define RIVR_RADIO_SX1262    1

/* ── No onboard display ───────────────────────────────────────────────── */
#define FEATURE_DISPLAY      0

/* ── Platform tag ─────────────────────────────────────────────────────── */
#define RIVR_PLATFORM_RP2040  1
