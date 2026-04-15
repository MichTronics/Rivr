/*
 * variants/esp32devkit_e22_900/config.h
 *
 * Build-variant header for:
 *   Board   : ESP32 DevKit V1 (Xtensa LX6, 4 MB flash)
 *   Radio   : EBYTE E22-900M30S or E22-900M33S (SX1262, HP PA, TCXO on DIO3)
 *
 * This file is force-included by PlatformIO for all esp32devkit_e22_900
 * environments (client and repeater alike) via the variant platformio.ini.
 * Role flags (RIVR_ROLE_CLIENT, RIVR_FABRIC_REPEATER, RIVR_BUILD_REPEATER)
 * are set by the environment build_flags in platformio.ini — NOT here.
 *
 * ─── Override guide ─────────────────────────────────────────────────────────
 * Any macro can be overridden by adding a -D flag BEFORE the -include line:
 *
 *   build_flags =
 *       -DRIVR_RF_FREQ_HZ=915000000   ; AU915 / US915
 *       -DPIN_SX1262_NSS=10           ; different CS pin
 *       -include ${PROJECT_DIR}/variants/esp32devkit_e22_900/config.h
 *
 * All macros use #ifndef guards so any earlier -D always wins.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#ifndef RIVR_VARIANT_ESP32DEVKIT_E22_900_H
#define RIVR_VARIANT_ESP32DEVKIT_E22_900_H

/* ── Radio chip selection ───────────────────────────────────────────────── */

/** Force SX1262 / SX126x driver path (EBYTE E22 uses SX1262). */
#ifndef RIVR_RADIO_SX1262
#  define RIVR_RADIO_SX1262 1
#endif

/* ── RF frequency ───────────────────────────────────────────────────────── */
/**
 * Default: 869.480 MHz (EU868 g3 high-power sub-band; 1 % duty cycle
 * enforced via dutycycle.c).
 *
 * Common overrides:
 *   -DRIVR_RF_FREQ_HZ=868100000   EU868 chan 0
 *   -DRIVR_RF_FREQ_HZ=915000000   AU915 / US915
 *   -DRIVR_RF_FREQ_HZ=923000000   AS923
 */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ 869480000UL
#endif

/* ── SX1262 GPIO pin mapping ────────────────────────────────────────────── */
/* Matches the standard ESP32 DevKit V1 + EBYTE E22 wiring used for RIVR.   */
/* Override any pin with -DPIN_SX1262_XXX=<gpio> before the -include.       */

#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK   18
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI  23
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO  19
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS    5
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY  32
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 25
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  33
#endif

/* ── Optional RF switch (RXEN / TXEN) ──────────────────────────────────── */
/* E22-900M30S and M33S use an internal PA+LNA switch driven by the MCU.   */
#ifndef RIVR_RFSWITCH_ENABLE
#  define RIVR_RFSWITCH_ENABLE 1
#endif
#ifndef PIN_SX1262_RXEN
#  define PIN_SX1262_RXEN  14
#endif
#ifndef PIN_SX1262_TXEN
#  define PIN_SX1262_TXEN  13
#endif

/* ── Status LED ─────────────────────────────────────────────────────────── */
#ifndef PIN_LED_STATUS
#  define PIN_LED_STATUS    2
#endif

/* ── Display ────────────────────────────────────────────────────────────── */
/* Enabled by default.  To build headless: -DFEATURE_DISPLAY=0              */
#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 1
#endif

/* ── DS18B20 OneWire temperature sensor ────────────────────────────────── */
/** Enable/disable DS18B20 support.  Set to 1 in platformio.ini build_flags. */
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
/** GPIO for DS18B20 data line (requires external 4.7 kΩ pull-up to 3.3 V). */
#ifndef PIN_DS18B20_ONEWIRE
#  define PIN_DS18B20_ONEWIRE 27
#endif

/* ── AM2302 (DHT22) humidity + temperature sensor ───────────────────────── */
/** Enable/disable AM2302 support.  Set to 1 in platformio.ini build_flags. */
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302 0
#endif
/** GPIO for AM2302 data line (requires external 5 kΩ pull-up to 3.3 V). */
#ifndef PIN_AM2302_DATA
#  define PIN_AM2302_DATA 26
#endif

/* ── Battery voltage ADC sensor ─────────────────────────────────────────── */
/** Enable/disable battery voltage measurement.  Set to 1 in platformio.ini. */
#ifndef RIVR_FEATURE_VBAT
#  define RIVR_FEATURE_VBAT 0
#endif
/**
 * GPIO connected to the battery voltage divider output (must be ADC1).
 * No built-in divider on a plain DevKit; wire externally using a free GPIO.
 * GPIO35 (ADC1_CH7) is a safe input-only choice.
 */
#ifndef PIN_ADC_VBAT
#  define PIN_ADC_VBAT 35
#endif
/** Voltage-divider ratio: V_bat = V_adc × RIVR_VBAT_DIV_NUM / RIVR_VBAT_DIV_DEN */
#ifndef RIVR_VBAT_DIV_NUM
#  define RIVR_VBAT_DIV_NUM 2
#endif
#ifndef RIVR_VBAT_DIV_DEN
#  define RIVR_VBAT_DIV_DEN 1
#endif

/* ── Sensor publish interval ────────────────────────────────────────────── */
/** Time between sensor reads and PKT_TELEMETRY transmissions (milliseconds). */
#ifndef RIVR_SENSOR_TX_MS
#  define RIVR_SENSOR_TX_MS 60000U
#endif

/**
 * Minimum spacing between successive PKT_TELEMETRY frames (milliseconds).
 *
 * Each LoRa node is half-duplex: while relaying packet N it cannot receive
 * packet N+1.  A relay at SF9 BW125 needs ~330 ms on-air + CSMA back-off
 * before the channel is clear again.  2 s gives comfortable headroom for
 * multi-hop meshes and EU868 duty-cycle recovery.
 *
 * Override with -DRIVR_SENSOR_PKT_INTERVAL_MS=<ms> if you need faster
 * telemetry (e.g. direct link, no relay).
 */
#ifndef RIVR_SENSOR_PKT_INTERVAL_MS
#  define RIVR_SENSOR_PKT_INTERVAL_MS 2000U
#endif

#endif /* RIVR_VARIANT_ESP32DEVKIT_E22_900_H */
