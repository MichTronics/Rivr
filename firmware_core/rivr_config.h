/**
 * @file  rivr_config.h
 * @brief Centralized configuration and feature-flag header for Rivr firmware.
 *
 * This header provides safe, documented defaults for every tuneable parameter
 * in the Rivr firmware.  All values use #ifndef guards — any flag set via:
 *   - a variant config.h  (variants/<device>/config.h, force-included by PlatformIO)
 *   - a -D build_flag in platformio.ini
 *   - the project platformio.ini shared [env] section
 *
 * …takes precedence over the defaults here.  You never need to modify this
 * file to change a setting: use the appropriate -D flag or variant config.h.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * QUICK REFERENCE
 * ──────────────────────────────────────────────────────────────────────────
 *
 * NODE ROLE
 *   RIVR_ROLE_CLIENT         1 = client node (send/receive chat, no relay)
 *   RIVR_FABRIC_REPEATER     1 = repeater (relay with congestion scoring)
 *   RIVR_BUILD_REPEATER      1 = repeater build (CLI banner, LED pattern)
 *   RIVR_ROLE_GATEWAY        1 = gateway (future: RF→IP bridge)
 *
 * RADIO
 *   RIVR_RF_FREQ_HZ          Centre frequency in Hz
 *   RF_SPREADING_FACTOR      LoRa SF (7–12)
 *   RF_BANDWIDTH_HZ          LoRa BW in Hz
 *   RF_CODING_RATE           LoRa CR denominator (5=4/5 … 8=4/8)
 *   RF_TX_POWER_DBM          TX power at chip output (SX1262: −9…22 dBm)
 *
 * MESH TIMING
 *   RIVR_BEACON_INTERVAL_MS  Beacon broadcast period (ms)
 *   RIVR_ROUTE_TIMEOUT_MS    Route cache entry lifetime (ms)
 *   RIVR_NEIGHBOR_TIMEOUT_MS Neighbor entry expiry (ms)
 *   RIVR_RETRY_TIMEOUT_MS    Unicast retry window (ms)
 *
 * MESH SIZES  (static BSS allocation — no heap)
 *   RIVR_MAX_NEIGHBORS       Neighbor table slots
 *   RIVR_MAX_ROUTES          Route cache slots
 *   RIVR_MAX_PENDING         Pending queue slots
 *   RIVR_MAX_RETRY           Retry table slots
 *
 * FEATURE FLAGS
 *   RIVR_FEATURE_CHAT        1 = PKT_CHAT origination + display enabled
 *   RIVR_FEATURE_RELAY       1 = frame relay enabled (flood + unicast)
 *   RIVR_FEATURE_METRICS     1 = @MET periodic metrics output enabled
 *   RIVR_FEATURE_DISPLAY     1 = SSD1306 OLED UI enabled
 *   RIVR_FEATURE_DEBUG_LOG   1 = verbose ESP_LOG* output (costs flash)
 *   RIVR_FEATURE_OTA         1 = PKT_PROG_PUSH OTA reception enabled
 *   RIVR_FEATURE_CRYPTO      1 = Ed25519 OTA sig verification
 *
 * SIMULATION
 *   RIVR_SIM_MODE            1 = no SPI/radio, inject synthetic frames
 *   RIVR_SIM_TX_PRINT        1 = print TX frames to UART (sim mode only)
 *
 * LOGGING
 *   RIVR_LOG_LEVEL           Compile-time log gate (see rivr_log.h)
 *                            0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=SILENT
 *
 * ──────────────────────────────────────────────────────────────────────────
 */

#pragma once

/* ══════════════════════════════════════════════════════════════════════════
 * NODE ROLE
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @defgroup rivr_cfg_role Node role flags
 * @{
 *
 * At most ONE role flag should be 1 at build time.  The PlatformIO variant
 * environments set the correct flag via build_flags.
 *
 * Default: full-relay node (safe for any hardware; listen + relay + accept
 * incoming chat, no origination from CLI).
 */

/** 1 = client node — enables serial CLI chat origination. */
#ifndef RIVR_ROLE_CLIENT
#  define RIVR_ROLE_CLIENT 0
#endif

/** 1 = Rivr Fabric congestion-aware relay scoring enabled.
 *  Set to 1 on dedicated repeater nodes. */
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER 0
#endif

/** 1 = repeater build — adjusts boot banner and LED pattern. */
#ifndef RIVR_BUILD_REPEATER
#  define RIVR_BUILD_REPEATER 0
#endif

/** 1 = gateway role — RF→IP bridge (stub in current firmware). */
#ifndef RIVR_ROLE_GATEWAY
#  define RIVR_ROLE_GATEWAY 0
#endif

/** @} */

/* ══════════════════════════════════════════════════════════════════════════
 * RADIO PARAMETERS
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @defgroup rivr_cfg_radio Radio air-interface parameters
 * @{
 *
 * All nodes on the same mesh must use identical air parameters.
 * Override via variant config.h or platformio.ini -D flags.
 */

/**
 * @brief LoRa centre frequency in Hz.
 *
 * EU868 g3 sub-band (high-power, 10 % duty-cycle): 869.480 MHz
 *
 * Common overrides:
 *   -DRIVR_RF_FREQ_HZ=868100000   EU868 channel 0
 *   -DRIVR_RF_FREQ_HZ=915000000   AU915 / US915
 *   -DRIVR_RF_FREQ_HZ=923000000   AS923
 *
 * Regulatory note: you are responsible for compliance with the radio
 * regulations in your jurisdiction.
 */
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ  869480000UL
#endif

/**
 * @brief LoRa spreading factor (7–12).
 *
 * Higher SF = longer range, lower data rate, higher time-on-air.
 * SF8 is a reasonable default balancing range and throughput.
 */
#ifndef RF_SPREADING_FACTOR
#  define RF_SPREADING_FACTOR  8
#endif

/**
 * @brief LoRa bandwidth in Hz.
 *
 * Supported exact values: 7800, 10400, 15600, 20800, 31250, 41700,
 * 62500, 125000, 250000, 500000.
 *
 * Prefer RF_BANDWIDTH_HZ for exact values. RF_BANDWIDTH_KHZ remains accepted
 * for backward compatibility and is mapped to the nearest supported exact
 * LoRa bandwidth.
 */
#if !defined(RF_BANDWIDTH_HZ) && defined(RF_BANDWIDTH_KHZ)
#  if   RF_BANDWIDTH_KHZ == 7
#    define RF_BANDWIDTH_HZ  7800u
#  elif RF_BANDWIDTH_KHZ == 10
#    define RF_BANDWIDTH_HZ 10400u
#  elif RF_BANDWIDTH_KHZ == 15
#    define RF_BANDWIDTH_HZ 15600u
#  elif RF_BANDWIDTH_KHZ == 20
#    define RF_BANDWIDTH_HZ 20800u
#  elif RF_BANDWIDTH_KHZ == 31
#    define RF_BANDWIDTH_HZ 31250u
#  elif RF_BANDWIDTH_KHZ == 41
#    define RF_BANDWIDTH_HZ 41700u
#  elif RF_BANDWIDTH_KHZ == 62
#    define RF_BANDWIDTH_HZ 62500u
#  elif RF_BANDWIDTH_KHZ == 125
#    define RF_BANDWIDTH_HZ 125000u
#  elif RF_BANDWIDTH_KHZ == 250
#    define RF_BANDWIDTH_HZ 250000u
#  elif RF_BANDWIDTH_KHZ == 500
#    define RF_BANDWIDTH_HZ 500000u
#  else
#    error "RF_BANDWIDTH_KHZ: unsupported legacy value"
#  endif
#endif

#ifndef RF_BANDWIDTH_HZ
#  define RF_BANDWIDTH_HZ  62500u
#endif

#if   RF_BANDWIDTH_HZ == 7800u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ  7u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "7.8"
#elif RF_BANDWIDTH_HZ == 10400u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 10u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "10.4"
#elif RF_BANDWIDTH_HZ == 15600u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 15u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "15.6"
#elif RF_BANDWIDTH_HZ == 20800u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 20u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "20.8"
#elif RF_BANDWIDTH_HZ == 31250u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 31u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "31.25"
#elif RF_BANDWIDTH_HZ == 41700u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 41u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "41.7"
#elif RF_BANDWIDTH_HZ == 62500u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 62u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "62.5"
#elif RF_BANDWIDTH_HZ == 125000u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 125u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "125"
#elif RF_BANDWIDTH_HZ == 250000u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 250u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "250"
#elif RF_BANDWIDTH_HZ == 500000u
#  ifndef RF_BANDWIDTH_KHZ
#    define RF_BANDWIDTH_KHZ 500u
#  endif
#  define RF_BANDWIDTH_DISPLAY_STR "500"
#else
#  error "RF_BANDWIDTH_HZ: unsupported — use a standard LoRa bandwidth"
#endif

/**
 * @brief LoRa coding rate denominator (5=4/5 … 8=4/8).
 *
 * Higher value adds more forward error correction at the cost of data rate.
 * CR 4/8 (value 8) maximises reliability for mesh control frames.
 */
#ifndef RF_CODING_RATE
#  define RF_CODING_RATE  8
#endif

/**
 * @brief TX power at the SX1262 chip output in dBm (−9 to 22).
 *
 * Note: EBYTE E22 modules have an external PA that adds ~8 dBm; a chip
 * output of 5 dBm yields ~+13 dBm effective radiated power, well within
 * EU868 limits.  The E22-900M30S can output up to +30 dBm ERP with
 * RF_TX_POWER_DBM = 22.
 *
 * Override per variant or regulatory requirement.
 */
#ifndef RF_TX_POWER_DBM
#  define RF_TX_POWER_DBM  5
#endif

/** @} */

/* ══════════════════════════════════════════════════════════════════════════
 * MESH TIMING
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @defgroup rivr_cfg_timing Mesh timing parameters
 * @{
 */

/**
 * @brief Beacon broadcast interval in milliseconds.
 *
 * Beacons advertise node presence and assist neighbor discovery.
 * 30 000 ms (30 s) is a good default for small meshes.
 * Reduce for faster neighbor discovery; increase to save airtime budget.
 */
#ifndef RIVR_BEACON_INTERVAL_MS
#  define RIVR_BEACON_INTERVAL_MS  30000U
#endif

/**
 * @brief Route cache entry lifetime in milliseconds.
 *
 * A route that has not been refreshed within this window is considered
 * stale and will be evicted on the next lookup.
 * 300 000 ms (5 min) suits low-traffic deployments.
 */
#ifndef RIVR_ROUTE_TIMEOUT_MS
#  define RIVR_ROUTE_TIMEOUT_MS  300000U
#endif

/**
 * @brief Neighbor table entry expiry in milliseconds.
 *
 * A neighbor that has not been heard within this window is marked STALE
 * and eventually removed from the table.
 * 120 000 ms (2 min) usually spans two or more missed beacons.
 */
#ifndef RIVR_NEIGHBOR_TIMEOUT_MS
#  define RIVR_NEIGHBOR_TIMEOUT_MS  120000U
#endif

/**
 * @brief Unicast retry window in milliseconds.
 *
 * Maximum time a pending-queue entry waits for an ACK before the retry
 * table attempts a retransmission.
 */
#ifndef RIVR_RETRY_TIMEOUT_MS
#  define RIVR_RETRY_TIMEOUT_MS  5000U
#endif

/** @} */

/* ══════════════════════════════════════════════════════════════════════════
 * MESH TABLE SIZES  (static BSS — no heap allocation)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @defgroup rivr_cfg_sizes Static table capacities
 * @{
 *
 * All tables are allocated in BSS at build time.  Increasing these values
 * increases RAM usage proportionally.  ESP32 has 320 kB SRAM available for
 * the application; the default configuration uses < 20 kB for these tables.
 */

/**
 * @brief Number of slots in the neighbor link-quality table.
 *
 * Each slot stores EWMA RSSI/SNR, seq-gap loss rate, and timestamps.
 * 16 slots ≈ 1.3 kB.
 */
#ifndef RIVR_MAX_NEIGHBORS
#  define RIVR_MAX_NEIGHBORS  16U
#endif

/**
 * @brief Number of slots in the unicast route cache.
 *
 * Each slot stores a destination node ID, next-hop, hop count, and score.
 * 16 slots ≈ 512 bytes.
 */
#ifndef RIVR_MAX_ROUTES
#  define RIVR_MAX_ROUTES  16U
#endif

/**
 * @brief Number of slots in the unicast pending queue.
 *
 * Frames awaiting ACK are held here until acknowledged or timed out.
 * 16 slots ≈ 4 kB.
 */
#ifndef RIVR_MAX_PENDING
#  define RIVR_MAX_PENDING  16U
#endif

/**
 * @brief Number of slots in the unicast retry table.
 *
 * Retry entries are created when a pending frame exhausts its ACK window.
 * 8 slots suffices for most deployments.
 */
#ifndef RIVR_MAX_RETRY
#  define RIVR_MAX_RETRY  8U
#endif

/** @} */

/* ══════════════════════════════════════════════════════════════════════════
 * FEATURE FLAGS
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @defgroup rivr_cfg_features Feature enable/disable flags
 * @{
 *
 * These flags gate optional subsystems at compile time.  Zero cost when
 * disabled — the conditional blocks compile to nothing.
 */

/**
 * @brief Enable PKT_CHAT origination from the serial CLI.
 *
 * Requires RIVR_ROLE_CLIENT = 1.  When enabled the CLI exposes the
 * "chat <message>" command and displays incoming CHAT frames.
 * Automatically enabled when RIVR_ROLE_CLIENT is set.
 */
#ifndef RIVR_FEATURE_CHAT
#  define RIVR_FEATURE_CHAT  RIVR_ROLE_CLIENT
#endif

/**
 * @brief Enable frame relay (flood dispatch + unicast cache lookup).
 *
 * All production firmware should keep this enabled.
 * Disable only for intentional leaf-only nodes.
 */
#ifndef RIVR_FEATURE_RELAY
#  define RIVR_FEATURE_RELAY  1
#endif

/**
 * @brief Enable periodic @MET JSON metrics output on the serial port.
 *
 * Outputs a compact JSON snapshot every RIVR_METRICS_INTERVAL_MS ms.
 * Useful for monitoring tools (tools/rivr-monitor).
 * Disable to reduce serial noise in production deployments.
 */
#ifndef RIVR_FEATURE_METRICS
#  define RIVR_FEATURE_METRICS  1
#endif

/**
 * @brief Enable verbose ESP_LOG* debug output.
 *
 * Equivalent to setting RIVR_LOG_LEVEL ≤ RIVR_LEVEL_DEBUG.
 * Adds significant flash usage; not recommended for production.
 */
#ifndef RIVR_FEATURE_DEBUG_LOG
#  define RIVR_FEATURE_DEBUG_LOG  0
#endif

/**
 * @brief Enable SSD1306 128×64 I²C OLED status display.
 *
 * Requires a physical OLED wired to I²C SDA/SCL (GPIO 21/22 by default).
 * Adds ~20 kB flash (display driver + FreeRTOS display task).
 */
#ifndef RIVR_FEATURE_DISPLAY
#  ifdef FEATURE_DISPLAY
#    define RIVR_FEATURE_DISPLAY  FEATURE_DISPLAY
#  else
#    define RIVR_FEATURE_DISPLAY  0
#  endif
#endif

/**
 * @brief Enable PKT_PROG_PUSH OTA program reception.
 *
 * When enabled the firmware accepts over-the-air RIVR program updates
 * delivered via PKT_PROG_PUSH.  Requires RIVR_FEATURE_CRYPTO = 1 for
 * Ed25519 signature verification.
 */
#ifndef RIVR_FEATURE_OTA
#  define RIVR_FEATURE_OTA  1
#endif

/**
 * @brief Enable Ed25519 OTA signature verification.
 *
 * When 0, all received PKT_PROG_PUSH frames are accepted without
 * verification (insecure — development only).
 * When 1, the firmware validates the Ed25519 signature against the
 * public key in firmware_core/rivr_pubkey.h.
 */
#ifndef RIVR_FEATURE_CRYPTO
#  ifdef RIVR_SIGNED_PROG
#    define RIVR_FEATURE_CRYPTO  1
#  else
#    define RIVR_FEATURE_CRYPTO  0
#  endif
#endif

/**
 * @brief Enable BLE local edge interface (Bluedroid UART-over-BLE bridge).
 *
 * When RIVR_FEATURE_BLE=1:
 *   • Bluedroid starts at boot and advertises as "RIVR-XXXX".
 *     Open BLE builds use a 120 s boot window; passkey-protected builds
 *     stay active until explicitly disabled, matching MeshCore-style
 *     companion pairing on demand.
 *   • A BLE client can write binary Rivr frames to inject them into the
 *     mesh (identical path to LoRa RX); the node notifies the client with
 *     every valid frame it receives from the radio.
 *   • Additional activation modes: BUTTON (5 min) and APP_REQUESTED
 *     (indefinite, triggered by mesh command).
 *
 * Requires:
 *   1. sdkconfig.ble appended to sdkconfig.defaults (see sdkconfig.ble).
 *   2. -DRIVR_FEATURE_BLE=1 in the environment's build_flags.
 *   3. CONFIG_BT_ENABLED=y in sdkconfig.
 *
 * See firmware_core/ble/rivr_ble.h for the full API.
 */
#ifndef RIVR_FEATURE_BLE
#  define RIVR_FEATURE_BLE  0
#endif

/**
 * @brief 6-digit BLE passkey for MITM-protected bonding.
 *
 * 0 (default) = open connection, no pairing required.  Any phone can connect
 * and exchange frames without entering a PIN.
 *
 * Non-zero = require passkey entry at first connection:
 *   • Firmware uses BLE_SM_IO_CAP_DISP_ONLY (Display Only).
 *     The active passkey is printed to the serial log and, when
 *     RIVR_FEATURE_DISPLAY=1, shown on the OLED while waiting for pairing.
 *   • By default the configured RIVR_BLE_PASSKEY is used as-is, so a
 *     build-time value like 123456 stays stable and can also be entered
 *     directly in the companion app.
 *   • Optional: set RIVR_BLE_RANDOM_PASSKEY=1 to generate a random 6-digit
 *     session passkey instead of using the configured build-time value.
 *   • LE Secure Connections (LESC) + MITM are enforced.
 *   • TX and RX characteristics require an encrypted link — any write or
 *     subscribe from an unauthenticated client is rejected with
 *     ATT_ERR_INSUFFICIENT_ENC.
 *   • The long-term key (LTK) is stored in NVS; subsequent reconnects
 *     re-encrypt silently without a re-entry prompt.
 *
 * Valid range: 1–999999.  Use a 6-digit value for best UX.
 * Example:  -DRIVR_BLE_PASSKEY=123456  (or set in variant config.h)
 *
 * Note: RIVR_FEATURE_BLE must be 1 for this to have any effect.
 */
#ifndef RIVR_BLE_PASSKEY
#  define RIVR_BLE_PASSKEY  0
#endif

/**
 * @brief Generate a random BLE passkey at boot instead of using RIVR_BLE_PASSKEY.
 *
 * Only applies when RIVR_BLE_PASSKEY != 0.  Default 0 keeps the configured
 * passkey stable, which matches the current companion-app pairing flow.
 */
#ifndef RIVR_BLE_RANDOM_PASSKEY
#  define RIVR_BLE_RANDOM_PASSKEY  0
#endif

/** @} */

/* ══════════════════════════════════════════════════════════════════════════
 * METRICS OUTPUT INTERVAL
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Period between automatic @MET JSON metric dumps in milliseconds.
 *
 * 60 000 ms (1 min) is a good default for remote monitoring.
 * Reduce to 10 000 for rapid metric streaming during development.
 * Set RIVR_FEATURE_METRICS = 0 to disable entirely.
 */
#ifndef RIVR_METRICS_INTERVAL_MS
#  define RIVR_METRICS_INTERVAL_MS  60000U
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * LOGGING LEVEL (compile-time gate — see rivr_log.h for full explanation)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Compile-time log level gate.
 *
 * 0 = TRACE  (most verbose — all macros active)
 * 1 = DEBUG
 * 2 = INFO   ← default
 * 3 = WARN
 * 4 = ERROR
 * 5 = SILENT (production minimal — almost no flash cost for logging)
 *
 * Override per environment in platformio.ini:
 *   build_flags = -DRIVR_LOG_LEVEL=2
 * or enable verbose debug output with RIVR_FEATURE_DEBUG_LOG:
 *   build_flags = -DRIVR_FEATURE_DEBUG_LOG=1
 */
#ifndef RIVR_LOG_LEVEL
#  if RIVR_FEATURE_DEBUG_LOG
#    define RIVR_LOG_LEVEL  1   /* DEBUG when debug-log feature is on */
#  else
#    define RIVR_LOG_LEVEL  2   /* INFO — default */
#  endif
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * SIMULATION MODE (no real SX1262/SX1276 hardware needed)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Run in software simulation mode (no SPI/radio hardware required).
 *
 * When 1:
 *   - Replaces radio_init/radio_start_rx with ring-buffer stubs
 *   - Injects synthetic SENSOR + CHAT frames before the main loop
 *   - RIVR_SIM_TX_PRINT = 1 logs TX frames to UART instead of transmitting
 *
 * Useful for testing the full routing and RIVR DSL stack on a bare ESP32
 * or in the host Rust replay harness (rivr_host).
 */
#ifndef RIVR_SIM_MODE
#  define RIVR_SIM_MODE  0
#endif

/** @brief Print TX frames to UART in simulation mode (no real transmission). */
#ifndef RIVR_SIM_TX_PRINT
#  define RIVR_SIM_TX_PRINT  0
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * DUTY-CYCLE (configuration guide)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * The duty-cycle budget is computed in dutycycle.h from two knobs:
 *
 *   DC_WINDOW_MS      = sliding window length in ms (default: 3 600 000 = 1 h)
 *   DC_DUTY_PCT_X10   = budget as 10× percent (default: 100 = 10 %)
 *
 * Override these in your variant platformio.ini to change the budget:
 *
 *   EU868 g1 (1 %)  :  -DDC_DUTY_PCT_X10=10
 *   EU868 g3 (10 %) :  -DDC_DUTY_PCT_X10=100   ← default
 *   EU433  (10 %)   :  -DDC_DUTY_PCT_X10=100   ← same formula
 *
 * See firmware_core/dutycycle.h for the complete formula.
 */
