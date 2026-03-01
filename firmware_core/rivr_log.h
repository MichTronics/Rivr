/**
 * @file  rivr_log.h
 * @brief RIVR structured logging — runtime mode + compile-time level gate.
 *
 * TWO-LAYER CONTROL
 * ─────────────────
 * 1. Compile-time: -D RIVR_LOG_LEVEL=<n>
 *      0 = TRACE  (most verbose — all macros active)
 *      1 = DEBUG  (TRACE suppressed at compile time)
 *      2 = INFO   (TRACE + DEBUG suppressed) ← DEFAULT
 *      3 = WARN   (only WARN + ERROR emitted)
 *      4 = ERROR  (only ERROR emitted)
 *      5 = SILENT (nothing emitted — production minimal)
 *
 *    This gate is resolved at compile time — zero runtime overhead for
 *    suppressed levels; the format strings are not even linked.
 *
 * 2. Runtime mode (g_rivr_log_mode): can further suppress INFO output
 *    without recompiling.  Useful for switching to @MET-only output via CLI.
 *
 * USAGE
 * ─────
 *   RIVR_LOGT("TAG", "trace detail: val=%d", val);   // level TRACE
 *   RIVR_LOGD("TAG", "debug: state=%d", state);      // level DEBUG
 *   RIVR_LOGI("TAG", "connected, node=0x%08x", id);  // level INFO
 *   RIVR_LOGW("TAG", "queue nearly full: %d", n);    // level WARN
 *   RIVR_LOGE("TAG", "SPI fault code=%d", err);      // level ERROR
 *
 * COMPILE-TIME DEFAULTS
 * ─────────────────────
 *   Debug build  : RIVR_LOG_LEVEL=0 (TRACE — all output)
 *   Release build: RIVR_LOG_LEVEL=2 (INFO — trace/debug stripped)
 *   Production   : RIVR_LOG_LEVEL=5 (SILENT — minimal flash usage)
 *
 *   Set in platformio.ini per environment:
 *     [env:client_esp32devkit_e22_900]
 *     build_flags = ... -D RIVR_LOG_LEVEL=2
 */

#pragma once
#include <stdint.h>
#include "esp_log.h"

/* ── Compile-time level definitions ─────────────────────────────────────── */

#define RIVR_LEVEL_TRACE   0
#define RIVR_LEVEL_DEBUG   1
#define RIVR_LEVEL_INFO    2
#define RIVR_LEVEL_WARN    3
#define RIVR_LEVEL_ERROR   4
#define RIVR_LEVEL_SILENT  5

/** Default to INFO if not specified at build time. */
#ifndef RIVR_LOG_LEVEL
#  define RIVR_LOG_LEVEL  RIVR_LEVEL_INFO
#endif

/* ── Runtime mode (for @MET-only / silent CLI switching) ────────────────── */

typedef enum {
    RIVR_LOG_DEBUG_MODE   = 0,  /**< Full ESP_LOG + @MET metrics           */
    RIVR_LOG_METRICS_MODE = 1,  /**< @MET metrics only (INFO suppressed)   */
    RIVR_LOG_SILENT_MODE  = 2,  /**< No output except WARN + ERROR         */
    /* Legacy aliases (backward compat) */
    RIVR_LOG_DEBUG   = 0,
    RIVR_LOG_METRICS = 1,
    RIVR_LOG_SILENT  = 2,
} rivr_log_mode_t;

extern rivr_log_mode_t g_rivr_log_mode;

void            rivr_log_set_mode(rivr_log_mode_t mode);
rivr_log_mode_t rivr_log_get_mode(void);

/* ── TRACE — most verbose, stripped in all non-debug builds ─────────────── */

#if RIVR_LOG_LEVEL <= RIVR_LEVEL_TRACE
#  define RIVR_LOGT(tag, fmt, ...) \
    do { \
        if (g_rivr_log_mode == RIVR_LOG_DEBUG) { \
            ESP_LOGD(tag, "[T] " fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#  define RIVR_LOGT(tag, fmt, ...)  do {} while (0)
#endif

/* ── DEBUG — stripped in INFO+ builds ──────────────────────────────────── */

#if RIVR_LOG_LEVEL <= RIVR_LEVEL_DEBUG
#  define RIVR_LOGD(tag, fmt, ...) \
    do { \
        if (g_rivr_log_mode == RIVR_LOG_DEBUG) { \
            ESP_LOGD(tag, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#  define RIVR_LOGD(tag, fmt, ...)  do {} while (0)
#endif

/* ── INFO — stripped in WARN+ builds, runtime-suppressible ─────────────── */

#if RIVR_LOG_LEVEL <= RIVR_LEVEL_INFO
#  define RIVR_LOGI(tag, fmt, ...) \
    do { \
        if (g_rivr_log_mode == RIVR_LOG_DEBUG) { \
            ESP_LOGI(tag, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#  define RIVR_LOGI(tag, fmt, ...)  do {} while (0)
#endif

/* ── WARN — always visible unless SILENT ────────────────────────────────── */

#if RIVR_LOG_LEVEL <= RIVR_LEVEL_WARN
#  define RIVR_LOGW(tag, fmt, ...) \
    do { \
        if (g_rivr_log_mode != RIVR_LOG_SILENT) { \
            ESP_LOGW(tag, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#  define RIVR_LOGW(tag, fmt, ...)  do {} while (0)
#endif

/* ── ERROR — always visible ─────────────────────────────────────────────── */

#if RIVR_LOG_LEVEL <= RIVR_LEVEL_ERROR
#  define RIVR_LOGE(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#  define RIVR_LOGE(tag, fmt, ...)  do {} while (0)
#endif
