/*
 * variants/lilygo_lora32_v21/config.h
 *
 * Build-variant header for:
 *   Board  : LilyGo LoRa32 V2.1_1.6  (TTGO T3 V1.6 / T3_V16)
 *            ESP32-D0WDQ6 · 4 MB flash · 520 kB SRAM
 *   Radio  : Integrated SX1276 (HF variant: 868/915 MHz)
 *            or SX1278 (LF variant: 433 MHz) — same register map.
 *   OLED   : SSD1306 128×64 I²C  (SDA=21, SCL=22  — same as default)
 *   LED    : GPIO 25 (active high)
 *
 * This file is force-included via PlatformIO build_flags:
 *   -include ${PROJECT_DIR}/variants/lilygo_lora32_v21/config.h
 *
 * ── Pin mapping  (LilyGo LoRa32 V2.1_1.6)  ──────────────────────────────
 *
 *  SPI (LoRa)          SX1276 signal   ESP32 GPIO
 *  ─────────────────── ─────────────── ──────────
 *  SCLK                SCK             5
 *  MOSI                MOSI            27
 *  MISO                MISO            19
 *  NSS / CS            NSS             18
 *  NRESET              RST             23
 *  DIO0  ← interrupt   RxDone/TxDone   26   ← mapped to PIN_SX1262_DIO1 slot
 *  DIO1  (RxTimeout)   –               33   ← not used by ISR
 *
 *  Display             Signal          GPIO
 *  ─────────────────── ─────────────── ──────────
 *  OLED SDA                            21   (default, no override needed)
 *  OLED SCL                            22   (default, no override needed)
 *
 *  Misc                Signal          GPIO
 *  ─────────────────── ─────────────── ──────────
 *  Status LED          Active-high     25
 *  Battery ADC                         35   (input-only, not used by RIVR)
 *  SD card CS                          13   (not used by RIVR)
 *
 * ── Override guide ───────────────────────────────────────────────────────
 *  All macros below use #ifndef guards.  Override any macro on the command
 *  line with -DMACRO=value BEFORE the -include directive:
 *
 *    build_flags =
 *        -DRIVR_RF_FREQ_HZ=868100000   ; EU868 channel 0 instead of 869.480
 *        -include variants/lilygo_lora32_v21/config.h
 *
 * ── Note on SX1262 macro namespace ───────────────────────────────────────
 *  PIN_SX1262_* macros are used by platform_esp32.c for GPIO init and by
 *  the radio driver for SPI/control pin access.  For the SX1276 we reuse
 *  this namespace: the SPI protocol differs but pin initialisation is the
 *  same.  PIN_SX1262_DIO1 is repurposed as the generic "radio interrupt"
 *  slot and is mapped to GPIO26 (= DIO0 on the SX1276 package).
 */

#ifndef RIVR_VARIANT_LILYGO_LORA32_V21_H
#define RIVR_VARIANT_LILYGO_LORA32_V21_H

/* ── Radio chip selection ────────────────────────────────────────────────── */

/** Enables radio_sx1276.c (selected by CMakeLists.txt via cmake var). */
#ifndef RIVR_RADIO_SX1276
#  define RIVR_RADIO_SX1276 1
#endif

/* ── No external RF switch ──────────────────────────────────────────────── */
/* The SX1276 manages its LNA/PA path internally; no RXEN/TXEN lines.       */
/* platform_sx1262_set_rxen() becomes a no-op when this is 0.               */
#ifndef RIVR_RFSWITCH_ENABLE
#  define RIVR_RFSWITCH_ENABLE 0
#endif

/* ── RF frequency ────────────────────────────────────────────────────────── */
/* Default: 869.480 MHz (EU868 sub-band g3, 1 % duty cycle, ≤27 dBm ERP).  */
/* Change to 868100000 for EU868 channel 0, 915000000 for AU915/US915, etc. */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ  869480000UL
#endif

/* ── LoRa modulation parameters ─────────────────────────────────────────── */
/* All overrideable with -D or earlier #define before the -include.          */

/** Spreading factor: 7 (fastest/shortest range) … 12 (slowest/longest). */
#ifndef RF_SPREADING_FACTOR
#  define RF_SPREADING_FACTOR  8u
#endif

/** Bandwidth in kHz: 7 / 10 / 15 / 20 / 31 / 41 / 62 / 125 / 250 / 500. */
#ifndef RF_BANDWIDTH_KHZ
#  define RF_BANDWIDTH_KHZ     125u
#endif

/** Coding-rate denominator (4/N): 5 = least overhead, 8 = most redundancy. */
#ifndef RF_CODING_RATE
#  define RF_CODING_RATE       8u
#endif

/**
 * TX output power in dBm for the SX1276 chip (PA_BOOST pin on LilyGo).
 * > 17 dBm: uses +20 dBm PA_DAC mode (needs ~200 mA from the 3.3V rail).
 * 2..17 dBm: standard PA_BOOST; Pout = 2 + OutputPower.
 * SX1276 achievable range: 2..17 dBm (standard) or 20 dBm (+20 dBm mode).
 */
#ifndef RF_TX_POWER_DBM
#  define RF_TX_POWER_DBM      20
#endif

/* ── SPI / control pins  (reusing PIN_SX1262_* namespace) ──────────────── */

/* SPI clock — LilyGo uses GPIO5 (different from the E22 DevKit wiring).    */
#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK    5
#endif
/* SPI MOSI — LilyGo uses GPIO27. */
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI  27
#endif
/* SPI MISO — GPIO19 (same as E22 DevKit). */
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO  19
#endif
/* Chip select — GPIO18. */
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS   18
#endif
/* NRESET — GPIO23 (active low, shared naming with SX1262 driver). */
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 23
#endif
/* "DIO1 slot" — wired to DIO0 on the SX1276 module (GPIO26).
 * Carries RxDone in RX mode and TxDone in TX mode.
 * platform_esp32.c attaches the ISR to this pin via gpio_isr_handler_add(). */
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  26
#endif
/* SX1276 DIO1 (RxTimeout) — GPIO33.  Informational; no ISR attached.      */
#ifndef PIN_SX1276_DIO1
#  define PIN_SX1276_DIO1  33
#endif

/* ── Status LED ──────────────────────────────────────────────────────────── */
/* LilyGo LoRa32 V2.1_1.6 has a user LED on GPIO25 (active high).          */
#ifndef PIN_LED_STATUS
#  define PIN_LED_STATUS   25
#endif

/* ── On-board SSD1306 OLED ────────────────────────────────────────────────
 * SDA=21 and SCL=22 are the defaults in display/display.h — no override
 * needed.  FEATURE_DISPLAY is enabled below so the OLED is used.           */
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY  1
#endif

#endif /* RIVR_VARIANT_LILYGO_LORA32_V21_H */
