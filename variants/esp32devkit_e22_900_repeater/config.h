/*
 * variants/esp32devkit_e22_900_repeater/config.h
 *
 * Build-variant header for:
 *   Board   : ESP32 DevKit V1 (Xtensa LX6, 4 MB flash)
 *   Radio   : EBYTE E22-900M30S or E22-900M33S (SX1262, HP PA, TCXO on DIO3)
 *   Role    : RIVR Fabric repeater — relays PKT_CHAT and PKT_DATA with
 *             congestion-aware DELAY / DROP (see firmware_core/rivr_fabric.h).
 *
 * This file is -include'd by PlatformIO via the [env:repeater_esp32devkit_e22_900]
 * build_flags entry.  It must be safe to include before any other header.
 *
 * ─── How to override ────────────────────────────────────────────────────────
 *  Pin or frequency can be overridden by passing an additional -D flag BEFORE
 *  -include on the compiler command line:
 *
 *    build_flags =
 *        ...
 *        -DRIVR_RF_FREQ_HZ=915000000           ; override to 915 MHz AU915
 *        -DPIN_SX1262_NSS=10                   ; different CS pin
 *        -include variants/esp32devkit_e22_900_repeater/config.h
 *
 *  All macros below use #ifndef guards so any -D on the command line wins.
 * ────────────────────────────────────────────────────────────────────────────
 */

#ifndef RIVR_VARIANT_ESP32DEVKIT_E22_900_REPEATER_H
#define RIVR_VARIANT_ESP32DEVKIT_E22_900_REPEATER_H

/* ── Role flags ─────────────────────────────────────────────────────────── */

/** This is a repeater-only build (no display, no CLI input expected). */
#ifndef RIVR_BUILD_REPEATER
#  define RIVR_BUILD_REPEATER 1
#endif

/**
 * Enable RIVR Fabric congestion-aware relay suppression.
 * Applies only to relayed PKT_CHAT and PKT_DATA frames; all other packet
 * types (ACK, BEACON, ROUTE_REQ/RPL, PROG_PUSH) always pass through.
 *
 * See firmware_core/rivr_fabric.h for scoring formula and policy thresholds.
 */
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER 1
#endif

/* ── Radio chip selection ───────────────────────────────────────────────── */

/** Force SX1262 / SX126x driver path. */
#ifndef RIVR_RADIO_SX1262
#  define RIVR_RADIO_SX1262 1
#endif

/* ── RF frequency ───────────────────────────────────────────────────────── */

/**
 * Default: 869.480 MHz (EU868 sub-band, legal at up to 27 dBm ERP with 1 %
 * duty cycle — enforced by dutycycle.c).
 *
 * Common overrides:
 *   -DRIVR_RF_FREQ_HZ=868100000   EU868 chan 0
 *   -DRIVR_RF_FREQ_HZ=915000000   AU915 / US915 centre (check local rules)
 *   -DRIVR_RF_FREQ_HZ=923000000   AS923
 */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ 869480000UL
#endif

/* ── SX1262 GPIO pin mapping ────────────────────────────────────────────── */
/* These match the standard ESP32 DevKit + EBYTE E22 wiring used for RIVR.  */
/* Override any pin with -DPIN_SX1262_XXX=<gpio> before the -include.       */

#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK   18
#endif
#ifndef PIN_SX1262_MOSI
#  define PIN_SX1262_MOSI  23
#endif
#ifndef PIN_SX1262_MISO
#  define PIN_SX1262_MISO  19
#endif
#ifndef PIN_SX1262_NSS
#  define PIN_SX1262_NSS    5
#endif
#ifndef PIN_SX1262_BUSY
#  define PIN_SX1262_BUSY  32
#endif
#ifndef PIN_SX1262_RESET
#  define PIN_SX1262_RESET 25
#endif
#ifndef PIN_SX1262_DIO1
#  define PIN_SX1262_DIO1  33
#endif

/* ── Optional RF switch (RXEN / TXEN) ──────────────────────────────────── */
/* E22-900M30S and M33S have an internal PA+LNA switch controlled by the    */
/* host MCU.  The driver in platform_esp32.c honours these pins when        */
/* RIVR_RFSWITCH_ENABLE is set.                                             */
#ifndef RIVR_RFSWITCH_ENABLE
#  define RIVR_RFSWITCH_ENABLE 1
#endif
#ifndef PIN_SX1262_RXEN
#  define PIN_SX1262_RXEN  14
#endif
#ifndef PIN_SX1262_TXEN
#  define PIN_SX1262_TXEN  13
#endif

/* ── Status LED ─────────────────────────────────────────────────────────── */
#ifndef PIN_LED_STATUS
#  define PIN_LED_STATUS    2
#endif

/* ── Display ────────────────────────────────────────────────────────────── */
/* Repeater nodes typically run headless; leave FEATURE_DISPLAY undefined   */
/* unless a display is physically wired.                                     */
/* To add a display: pass -DFEATURE_DISPLAY=1 before -include.              */

#endif /* RIVR_VARIANT_ESP32DEVKIT_E22_900_REPEATER_H */
