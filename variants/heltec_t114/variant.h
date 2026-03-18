/*
 * variant.h — Heltec T114 (nRF52840 + SX1262)
 *
 * Hardware-verified pin mapping, adapted from MeshCore project:
 * https://github.com/meshcore-dev/MeshCore/blob/main/variants/heltec_t114/variant.h
 *
 * Copyright (C) MeshCore contributors — MIT License
 */

#pragma once

#include "WVariant.h"

// ── Clock ──────────────────────────────────────────────────────────────────
#define USE_LFXO              // 32.768 kHz crystal oscillator
#define VARIANT_MCK           (64000000ul)

// ── I2C ────────────────────────────────────────────────────────────────────
#define WIRE_INTERFACES_COUNT (2)

// ── Power ──────────────────────────────────────────────────────────────────
#define NRF_APM
#define PIN_3V3_EN            (38)

#define BATTERY_PIN           (4)
#define ADC_MULTIPLIER        (4.90F)
#define ADC_RESOLUTION        (14)
#define BATTERY_SENSE_RES     (12)
#define AREF_VOLTAGE          (3.0)

// Power management boot protection threshold (millivolts)
#define PWRMGT_VOLTAGE_BOOTLOCK 3300
// LPCOMP wake: AIN2 = P0.04
#define PWRMGT_LPCOMP_AIN     2
#define PWRMGT_LPCOMP_REFSEL  1   // 2/8 VDD (~3.68-4.04 V)

// ── Pin count ──────────────────────────────────────────────────────────────
#define PINS_COUNT            (48)
#define NUM_DIGITAL_PINS      (48)
#define NUM_ANALOG_INPUTS     (1)
#define NUM_ANALOG_OUTPUTS    (0)

// ── UART ───────────────────────────────────────────────────────────────────
#define PIN_SERIAL1_RX        (37)
#define PIN_SERIAL1_TX        (39)

#define PIN_SERIAL2_RX        (9)
#define PIN_SERIAL2_TX        (10)

// ── I2C pins ───────────────────────────────────────────────────────────────
#define PIN_WIRE_SDA          (26)   // P0.26
#define PIN_WIRE_SCL          (27)   // P0.27

#define PIN_WIRE1_SDA         (7)    // P0.07
#define PIN_WIRE1_SCL         (8)    // P0.08 (note: Heltec docs say P0.07/P0.08)

// ── SPI (primary — SX1262 radio) ───────────────────────────────────────────
#define SPI_INTERFACES_COUNT  (2)

#define PIN_SPI_MISO          (23)
#define PIN_SPI_MOSI          (22)
#define PIN_SPI_SCK           (19)
#define PIN_SPI_NSS           (24)

// ── SPI1 (secondary — TFT display) ─────────────────────────────────────────
#define PIN_SPI1_MISO         (43)
#define PIN_SPI1_MOSI         (41)
#define PIN_SPI1_SCK          (40)

// ── LEDs ───────────────────────────────────────────────────────────────────
#define LED_BUILTIN           (35)
#define PIN_LED               LED_BUILTIN
#define LED_RED               LED_BUILTIN
#define LED_BLUE              (-1)    // no blue LED
#define LED_PIN               LED_BUILTIN
#define LED_STATE_ON          LOW

// ── NeoPixel ───────────────────────────────────────────────────────────────
#define PIN_NEOPIXEL          (14)
#define NEOPIXEL_NUM          (2)

// ── Buttons ────────────────────────────────────────────────────────────────
#define PIN_BUTTON1           (42)
#define BUTTON_PIN            PIN_BUTTON1
#define PIN_USER_BTN          BUTTON_PIN

// ── External flash (QSPI) ──────────────────────────────────────────────────
#define EXTERNAL_FLASH_DEVICES MX25R1635F
#define EXTERNAL_FLASH_USE_QSPI

// ── LoRa (SX1262) — used by RadioLib / BSP drivers ────────────────────────
#define USE_SX1262
#define LORA_CS               (24)
#define SX126X_DIO1           (20)
#define SX126X_BUSY           (17)
#define SX126X_RESET          (25)
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

// ── GPS ────────────────────────────────────────────────────────────────────
#define GPS_EN                (21)
#define GPS_RESET             (38)
#define PIN_GPS_RX            (39)   // bits going towards GPS
#define PIN_GPS_TX            (37)   // bits going towards CPU

// ── TFT (ST7789) ───────────────────────────────────────────────────────────
#define PIN_TFT_SCL           (40)
#define PIN_TFT_SDA           (41)
#define PIN_TFT_RST           (2)
#define PIN_TFT_VDD_CTL       (3)
#define PIN_TFT_LEDA_CTL      (15)
#define PIN_TFT_CS            (11)
#define PIN_TFT_DC            (12)
