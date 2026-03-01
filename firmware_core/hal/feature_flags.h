/**
 * @file  feature_flags.h
 * @brief Consolidated compile-time feature flag definitions for RIVR firmware.
 *
 * This is the SINGLE SOURCE OF TRUTH for all RIVR compile-time feature knobs.
 * Each flag defaults to the safest/smallest value and can be overridden via:
 *   a) Variant config.h (variants/<board>/config.h, -include'd by platformio.ini)
 *   b) Direct -D flag in platformio.ini build_flags
 *
 * All flags use #ifndef guards so any earlier definition always wins.
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ FLAG                        │ DEFAULT │ DESCRIPTION                  │
 * ├──────────────────────────────────────────────────────────────────────┤
 * │ RIVR_ROLE_CLIENT            │  0      │ Node is a client (chat only) │
 * │ RIVR_ROLE_REPEATER          │  0      │ Node relays CHAT+DATA        │
 * │ RIVR_FABRIC_REPEATER        │  0      │ Congestion fabric enabled    │
 * │ RIVR_RADIO_SX1262           │  1      │ Use SX1262/E22 driver        │
 * │ RIVR_RADIO_SX1276           │  0      │ Use SX1276/RFM95 driver      │
 * │ RIVR_FEATURE_ENCRYPTION     │  0      │ PSK frame encryption         │
 * │ RIVR_FEATURE_COMPRESSION    │  0      │ Payload compression (future) │
 * │ RIVR_FEATURE_BLE            │  0      │ BLE provisioning (future)    │
 * │ RIVR_SIGNED_PROG            │  0      │ OTA requires Ed25519 sig     │
 * │ RIVR_SIM_MODE               │  0      │ Simulation / host-test build │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * ROLE GUARD
 * ──────────
 * Exactly one of RIVR_ROLE_CLIENT / RIVR_ROLE_REPEATER should be set per
 * build.  The guard at the bottom of this file emits a compile-time warning
 * when neither is set (generic fallback — valid for tests/sim).
 *
 * RADIO GUARD
 * ───────────
 * Exactly one of RIVR_RADIO_SX1262 / RIVR_RADIO_SX1276 must be set.
 * A static_assert fires if neither is set in a non-simulation build.
 */

#pragma once

/* ── Role flags ─────────────────────────────────────────────────────────── */

/** Client node: sends/receives chat, does NOT relay CHAT or DATA. */
#ifndef RIVR_ROLE_CLIENT
#  define RIVR_ROLE_CLIENT       0
#endif

/** Repeater node: relays CHAT, DATA, and all control frames. */
#ifndef RIVR_ROLE_REPEATER
#  define RIVR_ROLE_REPEATER     0
#endif

/** Congestion-aware relay suppression (effective only when ROLE_REPEATER=1). */
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER   0
#endif

/* ── Radio chip selection ────────────────────────────────────────────────── */

/** Use SX1262 / EBYTE E22 driver (firmware_core/radio_sx1262.c). */
#ifndef RIVR_RADIO_SX1262
#  define RIVR_RADIO_SX1262      1
#endif

/** Use SX1276 / HopeRF RFM95 driver (firmware_core/radio_sx1276.c). */
#ifndef RIVR_RADIO_SX1276
#  define RIVR_RADIO_SX1276      0
#endif

/* ── Optional feature flags ─────────────────────────────────────────────── */

/**
 * PSK-AES-128-CTR frame-level encryption.
 * When enabled, set RIVR_PSK_HEX to a 32-hex-digit key string.
 * Example: -DRIVR_PSK_HEX=\"0102030405060708090a0b0c0d0e0f10\"
 */
#ifndef RIVR_FEATURE_ENCRYPTION
#  define RIVR_FEATURE_ENCRYPTION   0
#endif

/**
 * Payload compression (reserved for future use — miniz / heatshrink).
 * Setting this to 1 currently has no effect; the infrastructure is
 * in place for a future implementation.
 */
#ifndef RIVR_FEATURE_COMPRESSION
#  define RIVR_FEATURE_COMPRESSION  0
#endif

/**
 * BLE provisioning / companion app pairing (reserved for future use).
 * When enabled (future), the NimBLE stack will be initialised and a
 * provisioning GATT service will be advertised at startup.
 */
#ifndef RIVR_FEATURE_BLE
#  define RIVR_FEATURE_BLE          0
#endif

/* ── Security flags ─────────────────────────────────────────────────────── */

/**
 * Require Ed25519 signature on all PKT_PROG_PUSH OTA payloads.
 * Should be set in production builds; defaults off for dev convenience.
 * Implementation: firmware_core/rivr_ota.c + ed25519_verify.c
 */
#ifndef RIVR_SIGNED_PROG
#  define RIVR_SIGNED_PROG          0
#endif

/* ── Build mode flags ───────────────────────────────────────────────────── */

/** Simulation / host-test mode — disables hardware peripherals. */
#ifndef RIVR_SIM_MODE
#  define RIVR_SIM_MODE             0
#endif

/* ── Compile-time guards ─────────────────────────────────────────────────── */

#if RIVR_RADIO_SX1262 && RIVR_RADIO_SX1276
#  error "RIVR: Both RIVR_RADIO_SX1262 and RIVR_RADIO_SX1276 are set — pick one."
#endif

#if RIVR_ROLE_CLIENT && RIVR_ROLE_REPEATER
#  error "RIVR: Both RIVR_ROLE_CLIENT and RIVR_ROLE_REPEATER are set — pick one."
#endif

/* Warn (not error) when no role is set — valid for tests / generic builds */
#if !RIVR_ROLE_CLIENT && !RIVR_ROLE_REPEATER && !RIVR_SIM_MODE
#  pragma message("RIVR: No role flag set (RIVR_ROLE_CLIENT / RIVR_ROLE_REPEATER). Using generic defaults.")
#endif

/* ── Feature string (for build_info banner) ─────────────────────────────── */

#if RIVR_FEATURE_ENCRYPTION
#  define _RIVR_FEAT_ENC  "+enc"
#else
#  define _RIVR_FEAT_ENC  ""
#endif

#if RIVR_FEATURE_COMPRESSION
#  define _RIVR_FEAT_COMP "+comp"
#else
#  define _RIVR_FEAT_COMP ""
#endif

#if RIVR_FEATURE_BLE
#  define _RIVR_FEAT_BLE  "+ble"
#else
#  define _RIVR_FEAT_BLE  ""
#endif

/** Concatenated feature tag string, e.g. "+enc+ble" — for build_info banner. */
#define RIVR_FEATURE_TAG_STR  (_RIVR_FEAT_ENC _RIVR_FEAT_COMP _RIVR_FEAT_BLE)
