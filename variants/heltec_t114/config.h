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
 * │  Pin mapping hardware-verified against MeshCore project:               │
 * │  https://github.com/meshcore-dev/MeshCore/tree/main/variants/heltec_t114│
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * SX1262 SPI wiring (nRF52840 GPIO):
 *   SCK   = P0.19  (Arduino pin 19 = PIN_SPI_SCK)
 *   MOSI  = P0.22  (Arduino pin 22 = PIN_SPI_MOSI)
 *   MISO  = P0.23  (Arduino pin 23 = PIN_SPI_MISO)
 *   NSS   = P0.24  (chip select, active low)
 *   BUSY  = P0.17  (SX1262 busy flag, active high)
 *   RESET = P0.25  (NRST, active low)
 *   DIO1  = P0.20  (IRQ: TxDone | RxDone | Timeout)
 */

#pragma once

/* ── SX1262 SPI / control pins ────────────────────────────────────────── */
#define PIN_SX1262_SCK      19   /* P0.19 – SPIM SCK      */
#define PIN_SX1262_MOSI     22   /* P0.22 – SPIM MOSI     */
#define PIN_SX1262_MISO     23   /* P0.23 – SPIM MISO     */
#define PIN_SX1262_NSS      24   /* P0.24 – chip select   */
#define PIN_SX1262_BUSY     17   /* P0.17 – busy flag     */
#define PIN_SX1262_RESET    25   /* P0.25 – hardware reset */
#define PIN_SX1262_DIO1     20   /* P0.20 – IRQ line      */

/* RF switch: T114 uses DIO2 as internal RF switch (no external GPIO needed) */
#define RIVR_RFSWITCH_ENABLE   0

/* ── Status LED ───────────────────────────────────────────────────────── */
#define PIN_LED_STATUS      35   /* P1.03 – onboard LED (active LOW)      */

/* ── Radio driver selection ───────────────────────────────────────────── */
#define RIVR_RADIO_SX1262    1

/* ── OLED display: T114 does not have an onboard display ─────────────── */
#define FEATURE_DISPLAY      0

/* ── Platform tag ─────────────────────────────────────────────────────── */
#define RIVR_PLATFORM_NRF52840  1
