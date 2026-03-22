/**
 * @file  variants/seeed_t1000_e/config.h
 * @brief SenseCAP T1000-E scaffold for Rivr.
 *
 * Pin map adapted from MeshCore's T1000-E variant and Seeed's T1000-E docs.
 *
 * IMPORTANT:
 * This variant uses an experimental nRF52 + RadioLib-based LR1110 driver.
 * It is derived from MeshCore's T1000-E bring-up and still needs on-device
 * validation before it should be treated as production-ready.
 */

#pragma once

#define RIVR_PLATFORM_NRF52840 1
#define RIVR_PLATFORM_NAME     "Seeed T1000-E"
#define FEATURE_DISPLAY        0
#define RIVR_RADIO_SX1262      0
#define RIVR_RADIO_SX1276      0
#define RIVR_RADIO_LR1110      1
#define RIVR_RFSWITCH_ENABLE   0

/* Board peripherals discovered from MeshCore's T1000-E variant. */
#define PIN_LED_STATUS         24  /* P0.24, active HIGH */
#define PIN_USER_BUTTON        6   /* P0.06 */
#ifndef PIN_3V3_EN
#  define PIN_3V3_EN           38  /* P1.06 */
#endif
#define PIN_SENSOR_EN          4   /* P0.04 */
#define PIN_BUZZER             25  /* P0.25 */
#define PIN_BUZZER_EN          37  /* P1.05 */
#define PIN_GPS_EN             43  /* P1.11 */
#define PIN_GPS_RESET          47  /* P1.15 */

/* LR1110 radio pins. Also mapped onto the shared PIN_SX1262_* names so the
 * generic nRF52 platform HAL can still set up SPI/GPIO safely. */
#define PIN_LR1110_BUSY        7   /* P0.07 */
#define PIN_LR1110_SCK         11  /* P0.11 */
#define PIN_LR1110_NSS         12  /* P0.12 */
#define PIN_LR1110_MOSI        41  /* P1.09 */
#define PIN_LR1110_MISO        40  /* P1.08 */
#define PIN_LR1110_DIO1        33  /* P1.01 */
#define PIN_LR1110_RESET       42  /* P1.10 */
#define PIN_SX1262_BUSY        PIN_LR1110_BUSY
#define PIN_SX1262_SCK         PIN_LR1110_SCK
#define PIN_SX1262_NSS         PIN_LR1110_NSS
#define PIN_SX1262_MOSI        PIN_LR1110_MOSI
#define PIN_SX1262_MISO        PIN_LR1110_MISO
#define PIN_SX1262_DIO1        PIN_LR1110_DIO1
#define PIN_SX1262_RESET       PIN_LR1110_RESET
