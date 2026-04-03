/**
 * @file  firmware_core/platform_esp32.h  (RP2040 compat shim)
 * @brief Redirect ESP32 platform includes to the RP2040 HAL.
 */

#ifndef RIVR_COMPAT_PLATFORM_ESP32_H
#define RIVR_COMPAT_PLATFORM_ESP32_H

#include "../../firmware_core/platform_rp2040.h"

#ifndef UART_CLI_BAUD
#  define UART_CLI_BAUD 115200UL
#endif

#endif /* RIVR_COMPAT_PLATFORM_ESP32_H */
