/*
 * variants/seeed_xiao_sx1262/config.h
 *
 * Build-variant header for:
 *   Board  : Seeed Studio XIAO ESP32S3 + Wio-SX1262 expansion board
 *            (ESP32-S3, 8 MB flash)
 *   Radio  : SX1262 (Wio-SX1262 module, TCXO on DIO3 @ 1.8 V, DIO2 as RF switch)
 *   OLED   : none by default (compatible with XIAO expansion board)
 *
 * Product page:
 *   https://www.seeedstudio.com/Wio-SX1262-with-XIAO-ESP32S3-p-5982.html
 *
 * This file is force-included via PlatformIO build_flags:
 *   -include ${PROJECT_DIR}/variants/seeed_xiao_sx1262/config.h
 *
 * ── Pin mapping  (XIAO ESP32S3 + Wio-SX1262 kit) ─────────────────────────
 *
 *  SPI (LoRa)          SX1262 signal   ESP32-S3 GPIO
 *  ─────────────────── ─────────────── ─────────────
 *  SCLK                SCK             7
 *  MOSI                MOSI            9
 *  MISO                MISO            8
 *  NSS / CS            NSS             41
 *  BUSY                BUSY            40
 *  NRESET              RST             42
 *  DIO1  ← interrupt   IRQ             39
 *
 *  The SX1262 uses DIO2 as the RF TX/RX switch internally — no external
 *  RXEN/TXEN GPIO lines are needed (RIVR_RFSWITCH_ENABLE=0).
 *  DIO3 supplies the 1.8 V TCXO reference (SetDio3AsTcxoCtrl handled in
 *  radio_sx1262.c, no extra macro needed here).
 *
 * ── Override guide ───────────────────────────────────────────────────────
 *  All macros below use #ifndef guards.  Override any macro on the command
 *  line with -DMACRO=value BEFORE the -include directive.
 */

#ifndef RIVR_VARIANT_SEEED_XIAO_SX1262_H
#define RIVR_VARIANT_SEEED_XIAO_SX1262_H

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
#  define PIN_SX1262_SCK    7
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI   9
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO   8
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS   41
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY  40
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 42
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  39
#endif

#endif /* RIVR_VARIANT_SEEED_XIAO_SX1262_H */
