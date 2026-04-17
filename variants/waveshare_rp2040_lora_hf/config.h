/**
 * @file  variants/waveshare_rp2040_lora_hf/config.h
 * @brief Pin assignments for the Waveshare RP2040-LoRa-HF.
 *
 * Mapping aligned with:
 * - Meshtastic `variants/rp2040/rp2040-lora/variant.h`
 * - MeshCore `variants/waveshare_rp2040_lora`
 *
 * Meshtastic also documents the RF switch detail that matters here:
 * SX1262 DIO2 drives the TX switch path, while GPIO17 is the complementary
 * RX-enable line for the PE4259 switch network.
 */

#pragma once

#define RIVR_PLATFORM_RP2040        1
#define RIVR_PLATFORM_NAME          "Waveshare RP2040-LoRa-HF"

#define PIN_SX1262_SCK              14
#define PIN_SX1262_MOSI             15
#define PIN_SX1262_MISO             24
#define PIN_SX1262_NSS              13
#define PIN_SX1262_BUSY             18
#define PIN_SX1262_RESET            23
#define PIN_SX1262_DIO1             16
#define PIN_SX1262_RXEN             17

#define PIN_LED_STATUS              25

#define RIVR_RFSWITCH_ENABLE        1

#define RIVR_RADIO_SX1262           1
#define FEATURE_DISPLAY             0

/* Waveshare RP2040-LoRa-HF uses a crystal, not a DIO3-driven TCXO. */
#define RIVR_SX1262_USE_DIO3_TCXO   0
#define RIVR_SX1262_TCXO_VOLTAGE    0x00
#define RIVR_SX1262_TCXO_DELAY_TICKS 0x000000

/* ── DS18B20 OneWire temperature sensor ─────────────────────────────────── */
/* Not present on this board; disabled by default. */
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
#ifndef PIN_DS18B20_ONEWIRE
#  define PIN_DS18B20_ONEWIRE 0
#endif

/* ── AM2302 (DHT22) humidity + temperature sensor ───────────────────────── */
/* Not present on this board; disabled by default. */
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302 0
#endif
#ifndef PIN_AM2302_DATA
#  define PIN_AM2302_DATA 0
#endif

/* ── Battery voltage ADC sensor ─────────────────────────────────────────── */
/* Not present on this board; disabled by default. */
#ifndef RIVR_FEATURE_VBAT
#  define RIVR_FEATURE_VBAT 0
#endif
#ifndef PIN_ADC_VBAT
#  define PIN_ADC_VBAT 0
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
