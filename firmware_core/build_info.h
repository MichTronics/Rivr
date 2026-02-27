/**
 * @file  build_info.h
 * @brief Compile-time build identity: git SHA, build time, variant,
 *        compiler version, feature flags, and radio profile.
 *
 * All strings are read-only string literals (no heap, no printf at
 * definition time).  The only output function is build_info_print_banner(),
 * which emits a single formatted line at boot (called once from main.c).
 *
 * For the CLI `info` / `supportpack` commands, use build_info_write_json()
 * which writes a self-contained JSON object into a caller-supplied buffer.
 *
 * INJECTION (platformio.ini per-env):
 *   -DRIVR_GIT_SHA=\"cf379aa\"         8-char short hash from `git rev-parse --short HEAD`
 *   -DRIVR_BUILD_ENV=\"client_esp32devkit_e22_900\"   PlatformIO env name
 *
 * Fallbacks:   RIVR_GIT_SHA defaults to "unknown"
 *              RIVR_BUILD_ENV defaults to "unknown"
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Injected-or-defaulted macros ──────────────────────────────────────── */

#ifndef RIVR_GIT_SHA
#  define RIVR_GIT_SHA "unknown"
#endif

#ifndef RIVR_BUILD_ENV
#  define RIVR_BUILD_ENV "unknown"
#endif

/* Compiler version string — always available via pre-defined macros */
#define RIVR_COMPILER_VER \
    "gcc-" __GNUC_STRING__ "." __GNUC_MINOR_STRING__ "." __GNUC_PATCHLEVEL_STRING__

/* Helper macros to stringify numeric compiler version tokens */
#define _RIVR_STR(x) #x
#define _RIVR_XSTR(x) _RIVR_STR(x)
#define __GNUC_STRING__          _RIVR_XSTR(__GNUC__)
#define __GNUC_MINOR_STRING__    _RIVR_XSTR(__GNUC_MINOR__)
#define __GNUC_PATCHLEVEL_STRING__ _RIVR_XSTR(__GNUC_PATCHLEVEL__)

/* ── Feature-flag summary strings ─────────────────────────────────────── *
 * Each evaluates to a short tag at compile time; zero runtime cost.        */

#if RIVR_ROLE_CLIENT
#  define _RIVR_ROLE_TAG "client"
#elif defined(RIVR_BUILD_REPEATER) && RIVR_BUILD_REPEATER
#  define _RIVR_ROLE_TAG "repeater"
#else
#  define _RIVR_ROLE_TAG "generic"
#endif

#if RIVR_FABRIC_REPEATER
#  define _RIVR_FABRIC_TAG "+fabric"
#else
#  define _RIVR_FABRIC_TAG ""
#endif

#if defined(RIVR_RADIO_SX1276) && RIVR_RADIO_SX1276
#  define _RIVR_RADIO_TAG "SX1276"
#else
#  define _RIVR_RADIO_TAG "SX1262"
#endif

#ifdef RIVR_SIM_MODE
#  define _RIVR_SIM_TAG "+sim"
#else
#  define _RIVR_SIM_TAG ""
#endif

/* ── Radio profile constants (mirrored from radio_sx1262.h without pulling
 *    the full header into every consumer of build_info.h) ──────────────── */

#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ 869480000UL
#endif
#ifndef RF_SPREADING_FACTOR
#  define RF_SPREADING_FACTOR 8u
#endif
#ifndef RF_BANDWIDTH_KHZ
#  define RF_BANDWIDTH_KHZ 125u
#endif
#ifndef RF_CODING_RATE
#  define RF_CODING_RATE 8u
#endif

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Print a single-line boot banner to stdout.
 *
 * Format (one line, no ESP_LOG prefix):
 *   [RIVR] env=client_esp32devkit_e22_900 sha=cf379aa built=Feb 27 2026 14:05:32
 *          role=client radio=SX1262 freq=869480000 SF8 BW125kHz CR4/8
 *          cc=gcc-14.2.0 flags=+fabric
 *
 * Called ONCE from app_main() after rivr_embed_init().
 */
void build_info_print_banner(void);

/**
 * @brief Write a JSON object with all build/radio/flag fields into @p buf.
 *
 * The resulting string is NUL-terminated and fits within @p buf_len bytes.
 * Truncated gracefully if the buffer is too small (always NUL-terminates).
 *
 * Used by the `supportpack` CLI command to produce a pasteable block.
 *
 * @param buf      Caller-supplied buffer (recommended: >= 320 bytes)
 * @param buf_len  Size of buf in bytes
 * @return         Number of characters written (excluding NUL)
 */
int build_info_write_json(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
