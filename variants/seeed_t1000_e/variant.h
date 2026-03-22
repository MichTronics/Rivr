/*
 * variant.h — Seeed SenseCAP T1000-E (nRF52840 + LR1110)
 *
 * Pin mapping adapted from MeshCore's T1000-E variant.
 */

#pragma once

#include "WVariant.h"

#define USE_LFXO
#define VARIANT_MCK           (64000000ul)

#define NRF_APM
#ifndef PIN_3V3_EN
#define PIN_3V3_EN            (38)
#endif

#define BATTERY_PIN           (2)
#define BATTERY_IMMUTABLE
#define ADC_MULTIPLIER        (2.0F)
#define ADC_RESOLUTION        (14)
#define BATTERY_SENSE_RES     (12)
#define AREF_VOLTAGE          (3.0)

#define EXT_CHRG_DETECT       (35)
#define EXT_PWR_DETECT        (5)

#define PINS_COUNT            (48)
#define NUM_DIGITAL_PINS      (48)
#define NUM_ANALOG_INPUTS     (6)
#define NUM_ANALOG_OUTPUTS    (0)

#define PIN_SERIAL1_RX        (14)
#define PIN_SERIAL1_TX        (13)
#define PIN_SERIAL2_RX        (17)
#define PIN_SERIAL2_TX        (16)

#define HAS_WIRE              (1)
#define WIRE_INTERFACES_COUNT (1)
#define PIN_WIRE_SDA          (26)
#define PIN_WIRE_SCL          (27)

#define SPI_INTERFACES_COUNT  (1)
#define PIN_SPI_MISO          (40)
#define PIN_SPI_MOSI          (41)
#define PIN_SPI_SCK           (11)
#define PIN_SPI_NSS           (12)

#define LED_BUILTIN           (-1)
#define LED_GREEN             (24)
#define LED_BLUE              (-1)   /* no blue LED on T1000-E — required by Bluefruit52Lib */
#define LED_RED               (-1)   /* no red  LED on T1000-E — required by Bluefruit52Lib */
#define LED_PIN               LED_GREEN
#define LED_STATE_ON          HIGH

#define PIN_BUTTON1           (6)
#define BUTTON_PIN            PIN_BUTTON1
#define PIN_USER_BTN          BUTTON_PIN

/* LR1110 radio */
#define LORA_DIO_1            (33)
#define LORA_NSS              (PIN_SPI_NSS)
#define LORA_RESET            (42)
#define LORA_BUSY             (7)
#define LORA_SCLK             (PIN_SPI_SCK)
#define LORA_MISO             (PIN_SPI_MISO)
#define LORA_MOSI             (PIN_SPI_MOSI)

#define GPS_EN                (43)
#define GPS_RESET             (47)
#define GPS_VRTC_EN           (8)
#define GPS_SLEEP_INT         (44)
#define GPS_RTC_INT           (15)

#define SENSOR_EN             (4)
#define TEMP_SENSOR           (31)
#define LUX_SENSOR            (29)

#define BUZZER_EN             (37)
#define BUZZER_PIN            (25)
