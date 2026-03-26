/*
 * variants/rpi_e22_900/config.h
 *
 * Build-variant header for:
 *   Board  : Raspberry Pi (any model with 40-pin header)
 *   Radio  : E22-900M30S (SX1262, external HP-PA, TCXO on DIO3,
 *             DIO2 as RF switch, RXEN/TXEN external GPIOs)
 *
 * This file is force-included by Makefile.linux via:
 *   -include variants/rpi_e22_900/config.h
 *
 * GPIO pin assignments and SPI device paths live in
 * firmware_core/platform_linux.h (shared with all Linux builds).
 *
 * ── RF / LoRa profile  (EU868 E22-900M30S defaults) ─────────────────────
 *
 *   Centre freq : 869.480 MHz
 *   Spreading   : SF8
 *   Bandwidth   : 62.5 kHz
 *   Coding rate : CR 4/8
 *   TX power    : +22 dBm (chip output; E22 PA adds ~8 dBm)
 */

#pragma once

/* ── Radio frequency ─────────────────────────────────────────────────────── */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ        869480000UL
#endif

/* ── LoRa modulation ─────────────────────────────────────────────────────── */
#ifndef RF_SPREADING_FACTOR
#  define RF_SPREADING_FACTOR    8
#endif

#ifndef RF_BANDWIDTH_HZ
#  define RF_BANDWIDTH_HZ        62500UL
#endif

/** CR denominator: 5=4/5, 6=4/6, 7=4/7, 8=4/8 */
#ifndef RF_CODING_RATE
#  define RF_CODING_RATE         8
#endif

#ifndef RF_TX_POWER_DBM
#  define RF_TX_POWER_DBM        22
#endif

/* ── Build identity ──────────────────────────────────────────────────────── */
#ifndef RIVR_BUILD_ENV
/* Overridden by Makefile.linux to linux_rpi_e22_900_repeater for ROLE=repeater */
#  define RIVR_BUILD_ENV         "linux_rpi_e22_900"
#endif
