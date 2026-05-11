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
#include "hal/feature_flags.h"  /* RIVR_SIM_MODE, RIVR_ROLE_CLIENT, RIVR_FABRIC_REPEATER … */
#include "rivr_config.h"

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

#define RIVR_VERSION_STR "0.2.1"

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
#elif RIVR_ROLE_REPEATER || (defined(RIVR_BUILD_REPEATER) && RIVR_BUILD_REPEATER)
#  define _RIVR_ROLE_TAG "repeater"
#elif RIVR_ROLE_GATEWAY
#  define _RIVR_ROLE_TAG "gateway"
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
#elif defined(RIVR_RADIO_LR1110) && RIVR_RADIO_LR1110
#  define _RIVR_RADIO_TAG "LR1110"
#else
#  define _RIVR_RADIO_TAG "SX1262"
#endif

#if RIVR_SIM_MODE
#  define _RIVR_SIM_TAG "+sim"
#else
#  define _RIVR_SIM_TAG ""
#endif

#if defined(RIVR_FEATURE_DS18B20) && RIVR_FEATURE_DS18B20 && defined(RIVR_FEATURE_AM2302) && RIVR_FEATURE_AM2302
#  define _RIVR_SENSORS_TAG "+ds18b20+am2302"
#elif defined(RIVR_FEATURE_DS18B20) && RIVR_FEATURE_DS18B20
#  define _RIVR_SENSORS_TAG "+ds18b20"
#elif defined(RIVR_FEATURE_AM2302) && RIVR_FEATURE_AM2302
#  define _RIVR_SENSORS_TAG "+am2302"
#else
#  define _RIVR_SENSORS_TAG ""
#endif

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Print a single-line boot banner to stdout.
 *
 * Format (one line, no ESP_LOG prefix):
 *   [RIVR] env=client_esp32devkit_e22_900 sha=cf379aa built=Feb 27 2026 14:05:32
 *          role=client radio=SX1262 freq=869480000 SF8 BW62.5kHz CR4/8
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

/**
 * @brief Write the full @SUPPORTPACK JSON block into @p buf.
 *
 * Extends build_info_write_json() with a routing summary, duty-cycle snapshot,
 * uptime, and RX/TX frame counts. The caller gathers live values from the
 * relevant modules and passes them in — no global coupling in this module.
 *
 * Output prefix printed separately by the caller: "@SUPPORTPACK "
 *
 * Recommended minimum buffer size: 768 bytes (Step-9 adds 7 extra metric
 * fields totalling ~160 additional characters at peak integer values).
 *
 * @param buf             Caller-supplied buffer
 * @param buf_len         Size of buf in bytes
 * @param neighbor_cnt    Live neighbour count (routing_neighbor_count())
 * @param route_cnt       Live route count (route_cache_count())
 * @param pending_cnt     Pending queue occupancy (pending_queue_count())
 * @param dc_remaining_us Duty-cycle remaining budget µs (dutycycle_remaining_us())
 * @param dc_used_us      Duty-cycle used this window µs (g_dc.used_us)
 * @param dc_blocked      TX attempts blocked by DC gate (g_dc.blocked_count)
 * @param uptime_ms       Node uptime in milliseconds (tb_millis())
 * @param rx_frames       Total received frames (g_rx_frame_count)
 * @param tx_frames       Total transmitted frames (g_tx_frame_count)
 * @return                Number of bytes written (excluding NUL)
 */
int build_info_write_supportpack(char    *buf,
                                 size_t   buf_len,
                                 uint8_t  neighbor_cnt,
                                 uint8_t  route_cnt,
                                 uint8_t  pending_cnt,
                                 uint64_t dc_remaining_us,
                                 uint64_t dc_used_us,
                                 uint32_t dc_blocked,
                                 uint32_t uptime_ms,
                                 uint32_t rx_frames,
                                 uint32_t tx_frames);

#ifdef __cplusplus
}
#endif
