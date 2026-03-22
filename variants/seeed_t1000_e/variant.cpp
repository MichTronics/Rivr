/*
 * variant.cpp — Seeed SenseCAP T1000-E (nRF52840)
 *
 * Adapted from MeshCore's T1000-E variant.
 */

#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

const uint32_t g_ADigitalPinMap[PINS_COUNT + 1] = {
  0, 1, 2, 3, 4, 5, 6, 7,
  8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23,
  24, 25, 26, 27, 28, 29, 30, 31,
  32, 33, 34, 35, 36, 37, 38, 39,
  40, 41, 42, 43, 44, 45, 46, 47,
  255,
};

void initVariant()
{
  pinMode(BATTERY_PIN, INPUT);
  pinMode(TEMP_SENSOR, INPUT);
  pinMode(LUX_SENSOR, INPUT);
  pinMode(EXT_CHRG_DETECT, INPUT);
  pinMode(EXT_PWR_DETECT, INPUT);
  pinMode(PIN_BUTTON1, INPUT);

  pinMode(PIN_3V3_EN, OUTPUT);
  pinMode(BUZZER_EN, OUTPUT);
  pinMode(SENSOR_EN, OUTPUT);
  pinMode(GPS_EN, OUTPUT);
  pinMode(GPS_RESET, OUTPUT);
  pinMode(GPS_VRTC_EN, OUTPUT);
  pinMode(GPS_SLEEP_INT, OUTPUT);
  pinMode(GPS_RTC_INT, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(PIN_3V3_EN, LOW);
  digitalWrite(BUZZER_EN, LOW);
  digitalWrite(SENSOR_EN, LOW);
  digitalWrite(GPS_EN, LOW);
  digitalWrite(GPS_RESET, LOW);
  digitalWrite(GPS_VRTC_EN, LOW);
  digitalWrite(GPS_SLEEP_INT, HIGH);
  digitalWrite(GPS_RTC_INT, LOW);
  digitalWrite(LED_PIN, LOW);
}
