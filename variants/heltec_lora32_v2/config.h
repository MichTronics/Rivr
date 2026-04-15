/*
 * variants/heltec_lora32_v2/config.h
 *
 * Build-variant header for:
 *   Board  : Heltec WiFi LoRa 32 V2  (ESP32-D0WDQ6, 8 MB flash)
 *   Radio  : SX1276 (onboard, HF 868/915 MHz)
 *   OLED   : SSD1306 128×64 I²C  (SDA=4, SCL=15, RST=16)
 *
 * This file is force-included via PlatformIO build_flags:
 *   -include ${PROJECT_DIR}/variants/heltec_lora32_v2/config.h
 *
 * ── Pin mapping  (Heltec WiFi LoRa 32 V2) ────────────────────────────────
 *
 *  SPI (LoRa)          SX1276 signal   ESP32 GPIO
 *  ─────────────────── ─────────────── ──────────
 *  SCLK                SCK             5
 *  MOSI                MOSI            27
 *  MISO                MISO            19
 *  NSS / CS            NSS             18
 *  NRESET              RST             14
 *  DIO0  ← interrupt   RxDone/TxDone   26   ← mapped to PIN_SX1262_DIO1 slot
 *  DIO1  (RxTimeout)   –               35   ← informational only
 *
 *  Display             Signal          GPIO
 *  ─────────────────── ─────────────── ──────────
 *  OLED SDA            I²C SDA         4
 *  OLED SCL            I²C SCL         15
 *  OLED RST            Reset           16
 *
 * ── Note on SX1262 macro namespace ───────────────────────────────────────
 *  PIN_SX1262_* macros are reused for the SX1276; see lilygo_lora32_v21
 *  for documentation.  PIN_SX1262_DIO1 is mapped to DIO0 (GPIO26) on the
 *  SX1276 package which carries RxDone/TxDone.
 *
 * ── Override guide ───────────────────────────────────────────────────────
 *  All macros below use #ifndef guards.  Override any macro on the command
 *  line with -DMACRO=value BEFORE the -include directive.
 */

#ifndef RIVR_VARIANT_HELTEC_LORA32_V2_H
#define RIVR_VARIANT_HELTEC_LORA32_V2_H

/* ── Radio chip selection ────────────────────────────────────────────────── */
/** Enables radio_sx1276.c (selected by CMakeLists.txt via cmake var). */
#ifndef RIVR_RADIO_SX1276
#  define RIVR_RADIO_SX1276 1
#endif

/* ── No external RF switch ──────────────────────────────────────────────── */
/* The SX1276 manages its LNA/PA path internally; no RXEN/TXEN lines.       */
#ifndef RIVR_RFSWITCH_ENABLE
#  define RIVR_RFSWITCH_ENABLE 0
#endif

/* ── RF frequency ────────────────────────────────────────────────────────── */
/* Default: 869.480 MHz (EU868 sub-band g3, 1 % duty cycle enforced).       */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ 869480000UL
#endif

/* ── SPI / control pins  (reusing PIN_SX1262_* namespace) ──────────────── */
#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK    5
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI  27
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO  19
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS   18
#endif
/* NRESET — GPIO14 (active low). */
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 14
#endif
/* "DIO1 slot" — wired to DIO0 on the SX1276 module (GPIO26).
 * Carries RxDone in RX mode and TxDone in TX mode.                         */
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  26
#endif
/* SX1276 DIO1 (RxTimeout) — GPIO35.  Informational; no ISR attached.      */
#ifndef PIN_SX1276_DIO1
#  define PIN_SX1276_DIO1  35
#endif

/* ── On-board SSD1306 OLED ────────────────────────────────────────────────
 * Non-default I²C pins on Heltec V2 (SDA=4, SCL=15); OLED RST on GPIO16.  */
#ifndef PIN_DISPLAY_SDA
#  define PIN_DISPLAY_SDA  4
#endif
#ifndef PIN_DISPLAY_SCL
#  define PIN_DISPLAY_SCL  15
#endif
#ifndef PIN_OLED_RESET
#  define PIN_OLED_RESET 16
#endif
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 1
#endif

/* ── DS18B20 OneWire temperature sensor ─────────────────────────────────── */
/* GPIO27/26 are used by SPI-MOSI/DIO1; use GPIO13 and GPIO25 instead.      */
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
#ifndef PIN_DS18B20_ONEWIRE
#  define PIN_DS18B20_ONEWIRE 13
#endif

/* ── AM2302 (DHT22) humidity + temperature sensor ───────────────────────── */
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302 0
#endif
#ifndef PIN_AM2302_DATA
#  define PIN_AM2302_DATA 25
#endif

/* ── Battery voltage ADC sensor ─────────────────────────────────────────── */
/** Enable/disable battery voltage measurement.  Set to 1 in platformio.ini. */
#ifndef RIVR_FEATURE_VBAT
#  define RIVR_FEATURE_VBAT 0
#endif
/**
 * Heltec LoRa32 V2: battery voltage sense is on GPIO13, which is ADC2_CH4.
 * ADC2 cannot be used alongside the radio co-processor (unreliable reads).
 * RIVR_FEATURE_VBAT is therefore not supported on this board; leave at 0.
 */
#ifndef PIN_ADC_VBAT
#  define PIN_ADC_VBAT 13   /* ADC2 — not usable; feature must stay 0 */
#endif
#ifndef RIVR_VBAT_DIV_NUM
#  define RIVR_VBAT_DIV_NUM 2
#endif
#ifndef RIVR_VBAT_DIV_DEN
#  define RIVR_VBAT_DIV_DEN 1
#endif

/* ── Sensor publish intervals (shared defaults) ─────────────────────────── */
#ifndef RIVR_SENSOR_TX_MS
#  define RIVR_SENSOR_TX_MS 60000U
#endif
#ifndef RIVR_SENSOR_PKT_INTERVAL_MS
#  define RIVR_SENSOR_PKT_INTERVAL_MS 2000U
#endif

#endif /* RIVR_VARIANT_HELTEC_LORA32_V2_H */
