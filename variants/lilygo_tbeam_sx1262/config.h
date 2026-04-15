/*
 * variants/lilygo_tbeam_sx1262/config.h
 *
 * Build-variant header for:
 *   Board  : LilyGo T-Beam v1.1 / v1.2  (ESP32, SX1262 module)
 *            ESP32-D0WDQ6 · 4 MB flash · AXP192 PMIC
 *   Radio  : SX1262 (TCXO, DIO2 as RF switch)
 *   GPS    : L76K / NEO-6M (UART1: TX=12, RX=34) — not used by Rivr firmware
 *
 * This file is force-included via PlatformIO build_flags:
 *   -include ${PROJECT_DIR}/variants/lilygo_tbeam_sx1262/config.h
 *
 * ── IMPORTANT: AXP192 PMIC ───────────────────────────────────────────────
 * The T-Beam uses an AXP192 power management IC (I²C addr 0x34, SDA=21,
 * SCL=22) to supply power rails to the SX1262 module.  The PMIC MUST be
 * initialised before the SX1262 can be used.
 *
 * RIVR_TBEAM_AXP192=1 is defined here as a compile-time signal.
 * platform_esp32.c must check this flag and call the AXP192 init sequence
 * before radio_init().  Until that support is added, this variant is
 * marked EXPERIMENTAL in the hardware table.
 *
 * ── Pin mapping  (LilyGo T-Beam v1.1 / v1.2 with SX1262) ────────────────
 *
 *  SPI (LoRa)          SX1262 signal   ESP32 GPIO
 *  ─────────────────── ─────────────── ──────────
 *  SCLK                SCK             5
 *  MOSI                MOSI            27
 *  MISO                MISO            19
 *  NSS / CS            NSS             18
 *  BUSY                BUSY            32
 *  NRESET              RST             23
 *  DIO1  ← interrupt   IRQ             33
 *
 *  The SX1262 uses DIO2 as the RF TX/RX switch internally — no external
 *  RXEN/TXEN GPIO lines are needed (RIVR_RFSWITCH_ENABLE=0).
 *
 *  Power / I²C         Signal          GPIO
 *  ─────────────────── ─────────────── ──────────
 *  AXP192 SDA          I²C SDA         21
 *  AXP192 SCL          I²C SCL         22
 *  AXP192 IRQ          PMU interrupt   35   (not used by Rivr)
 *
 *  GPS (UART1)         Signal          GPIO
 *  ─────────────────── ─────────────── ──────────
 *  GPS RX  (ESP→GPS)                   12   (not used by Rivr)
 *  GPS TX  (GPS→ESP)                   34   (not used by Rivr)
 *
 * ── Override guide ───────────────────────────────────────────────────────
 *  All macros below use #ifndef guards.  Override any macro on the command
 *  line with -DMACRO=value BEFORE the -include directive.
 */

#ifndef RIVR_VARIANT_LILYGO_TBEAM_SX1262_H
#define RIVR_VARIANT_LILYGO_TBEAM_SX1262_H

/* ── Radio chip selection ────────────────────────────────────────────────── */
#ifndef RIVR_RADIO_SX1262
#  define RIVR_RADIO_SX1262 1
#endif

/* ── No external RF switch (DIO2 drives the internal PA/LNA path) ────────── */
#ifndef RIVR_RFSWITCH_ENABLE
#  define RIVR_RFSWITCH_ENABLE 0
#endif

/* ── AXP192 PMIC enable flag ─────────────────────────────────────────────── */
/* When 1, platform_esp32.c must initialise the AXP192 before radio_init(). */
#ifndef RIVR_TBEAM_AXP192
#  define RIVR_TBEAM_AXP192 1
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
#  define PIN_SX1262_MOSI  27
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO  19
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS   18
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY  32
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 23
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  33
#endif

/* ── Display ─────────────────────────────────────────────────────────────── */
/* T-Beam does not include an onboard OLED by default; set to 0.            */
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 0
#endif

/* ── DS18B20 OneWire temperature sensor ─────────────────────────────────── */
/* GPIO27=MOSI, GPIO34=GPS-RX (input-only) — use GPIO13 (free).             */
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
#ifndef PIN_DS18B20_ONEWIRE
#  define PIN_DS18B20_ONEWIRE 13
#endif

/* ── AM2302 (DHT22) humidity + temperature sensor ───────────────────────── */
/* GPIO26 is free on the T-Beam SX1262 variant.                             */
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
 * T-Beam SX1262: battery voltage is managed by AXP192 PMIC (I2C 0x34).
 * Direct ADC measurement is not available — RIVR_FEATURE_VBAT not supported
 * on this board; leave at 0.
 */
#ifndef PIN_ADC_VBAT
#  define PIN_ADC_VBAT 0   /* AXP192 PMIC — no ADC pin available */
#endif
#ifndef RIVR_VBAT_DIV_NUM
#  define RIVR_VBAT_DIV_NUM 1
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

#endif /* RIVR_VARIANT_LILYGO_TBEAM_SX1262_H */
