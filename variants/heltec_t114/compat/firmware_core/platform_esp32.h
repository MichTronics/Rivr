/**
 * @file  firmware_core/platform_esp32.h  (nRF52840 compat shim)
 * @brief Redirect all platform_esp32.h consumers to platform_nrf52.h.
 *
 * rivr_sources.c and other shared files use:
 *   #include "../firmware_core/platform_esp32.h"
 * The nRF52 build puts this compat directory first in the include path so
 * this stub is found instead of the real platform_esp32.h.
 *
 * Key substituions vs the original:
 *   • UART_CLI_BAUD:       kept (same value)
 *   • Platform HAL funcs:  forwarded to platform_nrf52.h equivalents
 *   • g_spi_sx1262:        NOT declared — platform_nrf52.c uses Arduino SPI
 *   • driver/spi_master.h: NOT included (no ESP-IDF on nRF52)
 */

#ifndef RIVR_COMPAT_PLATFORM_ESP32_H
#define RIVR_COMPAT_PLATFORM_ESP32_H

/* ── Pull in the nRF52 HAL so callers get the correct function signatures ─── */
#include "../../firmware_core/platform_nrf52.h"

/* ── UART baud rate (used by rivr_sources.c / rivr_cli.c for CLI) ─────────── */
#ifndef UART_CLI_BAUD
#  define UART_CLI_BAUD  115200UL
#endif

/* ── Pin assignments: use the nRF52 values from config.h (already pulled in
 *   via platform_nrf52.h → rivr_config.h → config.h in the nRF52 build).    ──
 *   If the guard didn't fire we still need the same defines here.
 *   config.h is force-included via -include flag in the platformio.ini so     ──
 *   all PIN_SX1262_* are already defined before any file is compiled.         */

/* No additional pin macros needed — config.h provides them all. */

#endif /* RIVR_COMPAT_PLATFORM_ESP32_H */
