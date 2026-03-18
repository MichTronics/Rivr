/**
 * @file  variants/heltec_t114/compat/freertos/FreeRTOS.h
 * @brief Redirect to the real FreeRTOS.h supplied by the Adafruit nRF52 BSP.
 *
 * The Adafruit nRF52 Arduino framework ships FreeRTOS with headers at the
 * path <FreeRTOS.h> (no freertos/ prefix).  Shared Rivr files include
 * "freertos/FreeRTOS.h" (ESP-IDF style), so this shim bridges the gap.
 */

#pragma once
#include <FreeRTOS.h>
