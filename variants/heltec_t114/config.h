/**
 * @file  variants/heltec_t114/config.h
 * @brief Pin assignments for the Heltec T114 (nRF52840 + SX1262).
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  Heltec T114                                                            │
 * │  MCU : nRF52840 (ARM Cortex-M4F, 1 MB flash, 256 KB SRAM)              │
 * │  Radio: Semtech SX1262, +22 dBm onboard PA                             │
 * │  USB  : Native USB (nRF52 USBD) via USB CDC-ACM                        │
 * │                                                                         │
 * │  NOTE: Verify every pin against the official Heltec T114 schematic     │
 * │  before flashing.  These assignments are based on the published v1.0   │
 * │  hardware reference; revision B may differ.                            │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * SX1262 SPI wiring (nRF52840 GPIO port 0):
 *   SCK   = P0.16
 *   MOSI  = P0.14
 *   MISO  = P0.15
 *   NSS   = P0.13  (chip select, active low)
 *   BUSY  = P0.12  (SX1262 busy flag, active high)
 *   RESET = P0.11  (NRST, active low)
 *   DIO1  = P0.17  (IRQ: TxDone | RxDone | Timeout)
 */

#pragma once

/* ── SX1262 SPI / control pins ────────────────────────────────────────── */
#define PIN_SX1262_SCK      16   /* P0.16 – SPIM SCK      */
#define PIN_SX1262_MOSI     14   /* P0.14 – SPIM MOSI     */
#define PIN_SX1262_MISO     15   /* P0.15 – SPIM MISO     */
#define PIN_SX1262_NSS      13   /* P0.13 – chip select   */
#define PIN_SX1262_BUSY     12   /* P0.12 – busy flag     */
#define PIN_SX1262_RESET    11   /* P0.11 – hardware reset */
#define PIN_SX1262_DIO1     17   /* P0.17 – IRQ line      */

/* RF switch: T114 has no external RF switch, DIO2 drives the internal one */
#define RIVR_RFSWITCH_ENABLE   0

/* ── Status LED ───────────────────────────────────────────────────────── */
#define PIN_LED_STATUS       7   /* P0.07 – onboard LED (active high)     */

/* ── Radio driver selection ───────────────────────────────────────────── */
#define RIVR_RADIO_SX1262    1

/* ── OLED display: T114 does not have an onboard display ─────────────── */
#define FEATURE_DISPLAY      0

/* ── Platform tag ─────────────────────────────────────────────────────── */
#define RIVR_PLATFORM_NRF52840  1
