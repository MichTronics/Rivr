/**
 * @file  user_config_template.h
 * @brief User-facing configuration template for Rivr firmware.
 *
 * HOW TO USE
 * ──────────
 * 1. Copy this file to your variant's config.h:
 *      cp user_config_template.h variants/esp32devkit_e22_900/config.h
 *    or keep it as a standalone override included via build_flags:
 *      -include ${PROJECT_DIR}/user_config_template.h
 *
 * 2. Uncomment and edit only the settings you want to change.
 *    Every setting has a safe default — you do NOT need to set everything.
 *
 * 3. All macros use #ifndef guards, so any -D flag in platformio.ini
 *    also takes precedence over values set here.
 *
 * SETTINGS REFERENCE
 * ──────────────────
 * Full documentation for every parameter is in firmware_core/rivr_config.h.
 * This file covers the most common settings for new deployments.
 */

#pragma once

/* ══════════════════════════════════════════════════════════════════════════
 * 1. NODE IDENTITY
 *    Set the human-readable name and network identifier for this node.
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Node callsign (1-11 chars, A-Z a-z 0-9 dash).
 * Shown in the boot banner and neighbour table.
 * Can also be changed at runtime with:  set callsign <CS>
 *
 * Default compile-time value — override at runtime with NVS persistence.
 */
/* #define RIVR_DEFAULT_CALLSIGN  "NODE1" */

/**
 * Network ID (16-bit hex).  Only nodes with the same net ID exchange frames.
 * Change this if you are running multiple independent meshes on the same
 * frequency.  Can also be changed at runtime with:  set netid <HEX>
 *
 * Default: 0x0000 (shared / open mesh).
 */
/* #define RIVR_DEFAULT_NET_ID  0x0000 */

/* ══════════════════════════════════════════════════════════════════════════
 * 2. NODE ROLE
 *    Exactly ONE role should be active per build environment.
 *    Role flags are normally set per environment in platformio.ini;
 *    leave these commented out and use the named pio environments instead.
 *
 *    client_esp32devkit_e22_900   → RIVR_ROLE_CLIENT=1
 *    repeater_esp32devkit_e22_900 → RIVR_FABRIC_REPEATER=1, RIVR_BUILD_REPEATER=1
 * ══════════════════════════════════════════════════════════════════════════ */

/* #define RIVR_ROLE_CLIENT        1 */   /* send/receive chat, no relay */
/* #define RIVR_FABRIC_REPEATER    1 */   /* congestion-aware relay scoring */
/* #define RIVR_BUILD_REPEATER     1 */   /* repeater boot banner + LED pattern */

/* ══════════════════════════════════════════════════════════════════════════
 * 3. RADIO FREQUENCY
 *    Must be identical on every node in the same mesh.
 *    Default: 869.480 MHz (EU868 g3 high-power sub-band, 10 % duty cycle).
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Centre frequency in Hz.
 *
 * Common values:
 *   869480000UL   EU868 g3 (default — 10 % duty cycle, ≤ 1 W ERP)
 *   868100000UL   EU868 channel 0 (1 % duty cycle)
 *   915000000UL   AU915 / US915
 *   923000000UL   AS923
 *
 * ⚠ You are responsible for compliance with radio regulations in your
 *   jurisdiction before operating any transmitter.
 */
/* #define RIVR_RF_FREQ_HZ  869480000UL */

/* ══════════════════════════════════════════════════════════════════════════
 * 4. TX POWER
 *    SX1262 chip output in dBm (range: −9 to 22).
 *    The EBYTE E22-900M30S adds ~8 dBm via external PA; chip output of 5 dBm
 *    yields ~+13 dBm ERP, within EU868 limits.
 * ══════════════════════════════════════════════════════════════════════════ */

/* #define RF_TX_POWER_DBM  5 */   /* default: +5 dBm chip → ~+13 dBm ERP with E22 PA */

/* ══════════════════════════════════════════════════════════════════════════
 * 5. LoRa MODULATION PARAMETERS
 *    All nodes on the mesh MUST use the same SF, BW, and CR.
 *    Default: SF8, BW 125 kHz, CR 4/8 — balanced range and throughput.
 * ══════════════════════════════════════════════════════════════════════════ */

/* #define RF_SPREADING_FACTOR  8   */  /* 7–12; higher = more range, lower data rate */
/* #define RF_BANDWIDTH_KHZ     125 */  /* kHz; 125 is standard for LoRa mesh */
/* #define RF_CODING_RATE       8   */  /* CR denominator: 5=4/5 … 8=4/8 */

/* ══════════════════════════════════════════════════════════════════════════
 * 6. MESH TIMING
 *    Beacon and timeout intervals.  Defaults suit most deployments.
 * ══════════════════════════════════════════════════════════════════════════ */

/* #define RIVR_BEACON_INTERVAL_MS   30000U */   /* neighbour beacon period (ms) */
/* #define RIVR_ROUTE_TIMEOUT_MS    300000U */   /* route cache entry lifetime (ms) */
/* #define RIVR_NEIGHBOR_TIMEOUT_MS 120000U */   /* neighbour expiry (ms) */

/* ══════════════════════════════════════════════════════════════════════════
 * 7. FEATURES
 *    Enable / disable optional subsystems.
 * ══════════════════════════════════════════════════════════════════════════ */

/* #define RIVR_FEATURE_METRICS  1 */   /* 1 = emit @MET JSON every RIVR_METRICS_INTERVAL_MS */
/* #define RIVR_METRICS_INTERVAL_MS  60000U */  /* ms between @MET snapshots */

/* #define RIVR_FEATURE_DISPLAY  1 */   /* 1 = enable SSD1306 OLED UI (requires wired display) */

/* #define RIVR_FEATURE_OTA      1 */   /* 1 = accept PKT_PROG_PUSH OTA program updates */

/* ══════════════════════════════════════════════════════════════════════════
 * 8. LOGGING LEVEL
 *    Controls compile-time verbosity of RIVR_LOG* macros.
 *    0=TRACE (most verbose) … 5=SILENT (minimal flash cost).
 *    Default: 2=INFO.  Set 3=WARN for quiet production nodes.
 * ══════════════════════════════════════════════════════════════════════════ */

/* #define RIVR_LOG_LEVEL  2 */   /* 0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=SILENT */
