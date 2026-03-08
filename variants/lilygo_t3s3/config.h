/*
 * variants/lilygo_t3s3/config.h
 *
 * Build-variant header for:
 *   Board  : LilyGo T3-S3 v1.x  (ESP32-S3, 16 MB flash)
 *   Radio  : SX1262 (onboard, TCXO, DIO2 as RF switch)
 *   OLED   : SSD1306 128×64 I²C  (SDA=18, SCL=17, RST=21) — optional
 *
 * This file is force-included via PlatformIO build_flags:
 *   -include ${PROJECT_DIR}/variants/lilygo_t3s3/config.h
 *
 * ── Pin mapping  (LilyGo T3-S3 v1.x) ────────────────────────────────────
 *
 *  SPI (LoRa)          SX1262 signal   ESP32-S3 GPIO
 *  ─────────────────── ─────────────── ─────────────
 *  SCLK                SCK             5
 *  MOSI                MOSI            6
 *  MISO                MISO            3
 *  NSS / CS            NSS             7
 *  BUSY                BUSY            34
 *  NRESET              RST             8
 *  DIO1  ← interrupt   IRQ             33
 *
 *  The SX1262 uses DIO2 as the RF TX/RX switch internally — no external
 *  RXEN/TXEN GPIO lines are needed (RIVR_RFSWITCH_ENABLE=0).
 *
 *  Display             Signal          GPIO
 *  ─────────────────── ─────────────── ─────────────
 *  OLED SDA            I²C SDA         18
 *  OLED SCL            I²C SCL         17
 *  OLED RST            Reset           21
 *
 * ── Override guide ───────────────────────────────────────────────────────
 *  All macros below use #ifndef guards.  Override any macro on the command
 *  line with -DMACRO=value BEFORE the -include directive.
 */

#ifndef RIVR_VARIANT_LILYGO_T3S3_H
#define RIVR_VARIANT_LILYGO_T3S3_H

/* ── Radio chip selection ────────────────────────────────────────────────── */
#ifndef RIVR_RADIO_SX1262
#  define RIVR_RADIO_SX1262 1
#endif

/* ── No external RF switch (DIO2 drives the internal PA/LNA path) ────────── */
#ifndef RIVR_RFSWITCH_ENABLE
#  define RIVR_RFSWITCH_ENABLE 0
#endif

/* ── RF frequency ────────────────────────────────────────────────────────── */
/* Default: 869.480 MHz (EU868 sub-band g3, 1 % duty cycle enforced).       */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ 869480000UL
#endif

/* ── SX1262 GPIO pin mapping ─────────────────────────────────────────────── */
#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK    5
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI   6
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO   3
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS    7
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY  34
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET  8
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  33
#endif

/* ── On-board SSD1306 OLED ────────────────────────────────────────────────
 * Non-default I²C pins (SDA=18, SCL=17); OLED RST on GPIO21.               */
#ifndef PIN_OLED_SDA
#  define PIN_OLED_SDA  18
#endif
#ifndef PIN_OLED_SCL
#  define PIN_OLED_SCL  17
#endif
#ifndef PIN_OLED_RESET
#  define PIN_OLED_RESET 21
#endif
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 1
#endif

#endif /* RIVR_VARIANT_LILYGO_T3S3_H */
