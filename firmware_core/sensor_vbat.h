/**
 * @file  sensor_vbat.h
 * @brief Battery voltage measurement via ESP-IDF 5.x ADC oneshot driver.
 *
 * Reads a resistor-divider-scaled battery voltage from a single ADC1 GPIO
 * and returns the actual battery voltage in millivolts.
 *
 * Typical divider ratios (Vbat through a top/bottom resistor to GND):
 *   LilyGo LoRa32 v2.1  GPIO35  100 kΩ / 100 kΩ  → NUM=2, DEN=1
 *   Heltec LoRa32 v3/v4  GPIO1  100 kΩ / 100 kΩ  → NUM=2, DEN=1
 *   LilyGo T3-S3         GPIO4  100 kΩ / 100 kΩ  → NUM=2, DEN=1
 *
 * IMPORTANT: Only ADC1 pins are supported.  ADC2 pins (e.g. GPIO13 on
 * Heltec v2) are unreliable when WiFi/BLE is active and are rejected at
 * init time.
 *
 * Guard by #if RIVR_FEATURE_VBAT in all call sites.
 */
#pragma once

#include <stdint.h>

/**
 * @brief Initialise the battery voltage ADC.
 *
 * @param gpio_num  ADC1-capable GPIO.
 * @param div_num   Numerator of voltage-divider ratio (Vbat = Vadc × num/den).
 * @param div_den   Denominator of voltage-divider ratio.
 */
void vbat_init(int gpio_num, int div_num, int div_den);

/**
 * @brief Read battery voltage in millivolts.
 *
 * @return Battery voltage in mV (e.g. 3700 = 3.70 V), or 0 on error / not init.
 */
int32_t vbat_read_mv(void);
