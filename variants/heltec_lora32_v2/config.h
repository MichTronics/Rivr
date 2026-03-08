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
#ifndef PIN_OLED_SDA
#  define PIN_OLED_SDA  4
#endif
#ifndef PIN_OLED_SCL
#  define PIN_OLED_SCL  15
#endif
#ifndef PIN_OLED_RESET
#  define PIN_OLED_RESET 16
#endif
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 1
#endif

#endif /* RIVR_VARIANT_HELTEC_LORA32_V2_H */
