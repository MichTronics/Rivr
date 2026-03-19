/**
 * @file  variants/waveshare_rp2040_lora/compat/firmware_core/platform_esp32.h
 * @brief Redirect all platform_esp32.h consumers to platform_rp2040.h.
 *
 * rivr_sources.c and other shared files use:
 *   #include "../firmware_core/platform_esp32.h"
 * The RP2040 build puts this compat directory first in the include path so
 * this stub is found instead of the real platform_esp32.h.
 */

#ifndef RIVR_COMPAT_PLATFORM_ESP32_H
#define RIVR_COMPAT_PLATFORM_ESP32_H

/* ── Pull in the RP2040 HAL so callers get the correct function signatures ── */
#include "../../firmware_core/platform_rp2040.h"

/* ── UART baud rate (used by rivr_sources.c / rivr_cli.c for CLI) ─────────── */
#ifndef UART_CLI_BAUD
#  define UART_CLI_BAUD  115200UL
#endif

/* config.h is force-included via -include flag in platformio.ini so
 * all PIN_SX1262_* are already defined before any file is compiled.     */

#endif /* RIVR_COMPAT_PLATFORM_ESP32_H */
