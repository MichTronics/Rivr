/*
 * variant.h — RAK WisMesh / WisPocket (RAK4631)
 *
 * Pin mapping aligned with:
 * - Meshtastic firmware variant for RAK4631
 * - MeshCore RAK4631 bring-up
 */

#pragma once

#include "WVariant.h"

#define USE_LFXO
#define VARIANT_MCK           (64000000ul)

#define NRF_APM
#ifndef PIN_3V3_EN
#define PIN_3V3_EN            (34)
#endif

#define BATTERY_PIN           (5)
#define ADC_MULTIPLIER        (1.73F)
#define ADC_RESOLUTION        (14)
#define BATTERY_SENSE_RES     (12)
#define AREF_VOLTAGE          (3.0)

#define PINS_COUNT            (48)
#define NUM_DIGITAL_PINS      (48)
#define NUM_ANALOG_INPUTS     (6)
#define NUM_ANALOG_OUTPUTS    (0)

#define PIN_SERIAL1_RX        (15)
#define PIN_SERIAL1_TX        (16)
#define PIN_SERIAL2_RX        (8)
#define PIN_SERIAL2_TX        (6)

#define WIRE_INTERFACES_COUNT (1)
#define PIN_WIRE_SDA          (13)
#define PIN_WIRE_SCL          (14)

#define SPI_INTERFACES_COUNT  (2)
#define PIN_SPI_MISO          (45)
#define PIN_SPI_MOSI          (44)
#define PIN_SPI_SCK           (43)
#define PIN_SPI_NSS           (42)

#define PIN_SPI1_MISO         (29)
#define PIN_SPI1_MOSI         (30)
#define PIN_SPI1_SCK          (3)

#define LED_BUILTIN           (35)
#define LED_CONN              (36)
#define PIN_LED               LED_BUILTIN
#define LED_GREEN             LED_BUILTIN
#define LED_BLUE              LED_CONN
#define LED_PIN               LED_BUILTIN
#define LED_STATE_ON          HIGH

#define PIN_BUTTON1           (9)
#define BUTTON_PIN            PIN_BUTTON1
#define PIN_USER_BTN          BUTTON_PIN
#define BUTTON_NEED_PULLUP

#define EXTERNAL_FLASH_DEVICES IS25LP080D
#define EXTERNAL_FLASH_USE_QSPI

#define USE_SX1262
#define LORA_CS               (42)
#define SX126X_DIO1           (47)
#define SX126X_BUSY           (46)
#define SX126X_RESET          (38)
#define SX126X_POWER_EN       (37)
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

#define PIN_GPS_PPS           (17)
#define GPS_RX_PIN            PIN_SERIAL1_RX
#define GPS_TX_PIN            PIN_SERIAL1_TX
