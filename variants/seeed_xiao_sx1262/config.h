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

/* ── DS18B20 OneWire temperature sensor ─────────────────────────────────── */
/* XIAO ESP32-S3 pad D1 = GPIO2 (free, not used by Wio-SX1262 expansion).   */
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
#ifndef PIN_DS18B20_ONEWIRE
#  define PIN_DS18B20_ONEWIRE 2
#endif

/* ── AM2302 (DHT22) humidity + temperature sensor ───────────────────────── */
/* XIAO ESP32-S3 pad D2 = GPIO3 (free).                                      */
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302 0
#endif
#ifndef PIN_AM2302_DATA
#  define PIN_AM2302_DATA 3
#endif

/* ── Battery voltage ADC sensor ─────────────────────────────────────────── */
/** Enable/disable battery voltage measurement.  Set to 1 in platformio.ini. */
#ifndef RIVR_FEATURE_VBAT
#  define RIVR_FEATURE_VBAT 0
#endif
/**
 * XIAO ESP32-S3 / Wio-SX1262: no on-board battery voltage divider circuit.
 * Leave RIVR_FEATURE_VBAT=0 unless an external divider is wired to a free
 * ADC1 GPIO (e.g. GPIO5 = D4 on the XIAO expansion pads).
 */
#ifndef PIN_ADC_VBAT
#  define PIN_ADC_VBAT 0   /* No on-board circuit */
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

#endif /* RIVR_VARIANT_SEEED_XIAO_SX1262_H */
