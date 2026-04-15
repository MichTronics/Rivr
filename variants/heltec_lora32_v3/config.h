/*
 * variants/heltec_lora32_v3/config.h
 *
 * Build-variant header for:
 *   Board  : Heltec WiFi LoRa 32 V3  (ESP32-S3, 8 MB flash)
 *   Radio  : SX1262 (onboard, TCXO on DIO3, DIO2 as RF switch)
 *   OLED   : SSD1306 128×64 I²C  (SDA=17, SCL=18)
 *
 * This file is force-included via PlatformIO build_flags:
 *   -include ${PROJECT_DIR}/variants/heltec_lora32_v3/config.h
 *
 * ── Pin mapping  (Heltec WiFi LoRa 32 V3) ────────────────────────────────
 *
 *  SPI (LoRa)          SX1262 signal   ESP32-S3 GPIO
 *  ─────────────────── ─────────────── ─────────────
 *  SCLK                SCK             9
 *  MOSI                MOSI            10
 *  MISO                MISO            11
 *  NSS / CS            NSS             8
 *  BUSY                BUSY            13
 *  NRESET              RST             12
 *  DIO1  ← interrupt   IRQ             14
 *
 *  The SX1262 uses DIO2 as the RF TX/RX switch internally — no external
 *  RXEN/TXEN GPIO lines are needed (RIVR_RFSWITCH_ENABLE=0).
 *
 *  Display             Signal          GPIO
 *  ─────────────────── ─────────────── ─────────────
 *  OLED SDA            I²C SDA         17
 *  OLED SCL            I²C SCL         18
 *  VEXT_EN             Peripheral pwr  36   (active-low; asserted in platform_init)
 *
 * ── Override guide ───────────────────────────────────────────────────────
 *  All macros below use #ifndef guards.  Override any macro on the command
 *  line with -DMACRO=value BEFORE the -include directive.
 */

#ifndef RIVR_VARIANT_HELTEC_LORA32_V3_H
#define RIVR_VARIANT_HELTEC_LORA32_V3_H

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
#  define PIN_SX1262_SCK    9
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI  10
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO  11
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS    8
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY  13
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 12
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  14
#endif

/* ── VEXT peripheral power (active-low, Heltec V3) ─────────────────────── */
/* GPIO36 low = VEXT rail on → powers the OLED and external peripherals.    */
#ifndef PIN_VEXT_EN
#  define PIN_VEXT_EN  36
#endif

/* ── On-board SSD1306 OLED ────────────────────────────────────────────────
 * The I²C bus uses SDA=17, SCL=18 — these override the defaults in
 * display/display.h which assume SDA=21, SCL=22.
 * RST_OLED is GPIO 21 (from pins_arduino.h / Heltec V3 board definition).
 * It must be pulsed LOW→HIGH before the SSD1306 will respond to I2C.
 * FEATURE_DISPLAY=1 enables the OLED path in display.c.                    */
#ifndef PIN_DISPLAY_SDA
#  define PIN_DISPLAY_SDA 17
#endif
#ifndef PIN_DISPLAY_SCL
#  define PIN_DISPLAY_SCL 18
#endif
#ifndef PIN_DISPLAY_RST
#  define PIN_DISPLAY_RST 21
#endif
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 1
#endif

/* ── DS18B20 OneWire temperature sensor ─────────────────────────────────── */
/* GPIO4 is free on the expansion header (no SPI/I²C/UART conflict).        */
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
#ifndef PIN_DS18B20_ONEWIRE
#  define PIN_DS18B20_ONEWIRE 4
#endif

/* ── AM2302 (DHT22) humidity + temperature sensor ───────────────────────── */
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302 0
#endif
#ifndef PIN_AM2302_DATA
#  define PIN_AM2302_DATA 48
#endif

/* ── Battery voltage ADC sensor ─────────────────────────────────────────── */
/** Enable/disable battery voltage measurement.  Set to 1 in platformio.ini. */
#ifndef RIVR_FEATURE_VBAT
#  define RIVR_FEATURE_VBAT 0
#endif
/**
 * Heltec LoRa32 V3: battery sense on GPIO1 (ADC1_CH0, ESP32-S3).
 * On-board 100 kΩ / 100 kΩ voltage divider → V_bat = V_adc × 2.
 */
#ifndef PIN_ADC_VBAT
#  define PIN_ADC_VBAT 1
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

#endif /* RIVR_VARIANT_HELTEC_LORA32_V3_H */
