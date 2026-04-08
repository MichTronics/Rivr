/**
 * @file  variants/rak4631/config.h
 * @brief Pin assignments for the RAK WisMesh / WisPocket (RAK4631).
 *
 * Pin map aligned with the Meshtastic RAK4631 variant and MeshCore's RAK4631
 * bring-up for the RAK WisMesh / WisPocket hardware family. The board uses
 * the SX1262's DIO2 and DIO3 helpers for RF switch and TCXO control, so no
 * external TX/RX enable GPIO is configured here.
 */

#pragma once

#define RIVR_PLATFORM_NRF52840 1
#define RIVR_PLATFORM_NAME     "RAK WisMesh / WisPocket (RAK4631)"

/* SX1262 wiring on the RAK4631 module / WisBlock base. */
#define PIN_SX1262_NSS         42  /* P1.10 */
#define PIN_SX1262_SCK         43  /* P1.11 */
#define PIN_SX1262_MOSI        44  /* P1.12 */
#define PIN_SX1262_MISO        45  /* P1.13 */
#define PIN_SX1262_BUSY        46  /* P1.14 */
#define PIN_SX1262_DIO1        47  /* P1.15 */
#define PIN_SX1262_RESET       38  /* P1.06 */

/* DIO2 drives the RF switch on this board. */
#define RIVR_RFSWITCH_ENABLE   0

/* Board peripherals. */
#define PIN_LED_STATUS         35  /* P1.03, active HIGH */
#define PIN_USER_BUTTON        9   /* P0.09, pull-up button */
#define PIN_3V3_EN             34  /* P1.02, switched 3V3 rail */

/* Radio driver selection. */
#define RIVR_RADIO_SX1262      1

/* WisPocket OLED: SSD1306 on the default WisBlock I2C bus. */
#define FEATURE_DISPLAY        1
#define PIN_DISPLAY_SDA        13
#define PIN_DISPLAY_SCL        14
#define DISPLAY_I2C_ADDR       0x3C
