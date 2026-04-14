/**
 * @file  sensor_am2302.h
 * @brief AM2302 (DHT22) humidity + temperature sensor — single-bus driver.
 *
 * Usage:
 *   1. Call am2302_init(&ctx, gpio_num) once at startup.
 *   2. Call am2302_tick(&ctx, now_ms) from the main loop (~every 10 ms).
 *      Returns true when a fresh reading is available (at most once per
 *      RIVR_AM2302_MIN_INTERVAL_MS, default 2 s, enforced internally).
 *   3. Retrieve results with am2302_get_rh_x100() / am2302_get_temp_x100().
 *
 * When RIVR_FEATURE_AM2302 == 0, all functions compile to no-ops / constants.
 * Requires an external 5 kΩ (3.3 V tolerant) pull-up on the data pin.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Feature guard ───────────────────────────────────────────────────────── */
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302 0
#endif

/** Minimum time between two reads (AM2302 datasheet: ≥ 2 s). */
#ifndef RIVR_AM2302_MIN_INTERVAL_MS
#  define RIVR_AM2302_MIN_INTERVAL_MS 2000U
#endif

#if RIVR_FEATURE_AM2302

/* ── Context ─────────────────────────────────────────────────────────────── */
typedef struct {
    int      gpio;          /**< GPIO number (open-drain)    */
    uint32_t last_read_ms;  /**< Timestamp of last read attempt */
    int32_t  rh_x100;       /**< Relative humidity × 100 (%RH) */
    int32_t  temp_x100;     /**< Temperature × 100 (°C)     */
    bool     valid;         /**< At least one valid reading  */
} am2302_ctx_t;

/**
 * @brief Configure the GPIO pin for AM2302 operation.
 * @param ctx   Uninitialised context struct.
 * @param gpio  GPIO number; must have an external pull-up resistor.
 */
void am2302_init(am2302_ctx_t *ctx, int gpio);

/**
 * @brief Drive the sensor read cycle.
 *
 * Call every loop tick (~10 ms).  When the internal interval expires, this
 * function issues the AM2302 start signal, reads 40 bits (interrupts disabled
 * for ~5 ms during actual bit-sampling), validates the checksum, stores the
 * result, and returns true.  Returns false on every other call.
 *
 * @param ctx    Initialised context.
 * @param now_ms Current time in milliseconds (monotonic).
 * @return true  A fresh, valid reading was stored this call.
 * @return false No new reading (interval not elapsed, or read error).
 */
bool am2302_tick(am2302_ctx_t *ctx, uint32_t now_ms);

/**
 * @brief Return last valid relative-humidity reading.
 * @return Humidity × 100 in %RH (e.g. 5512 → 55.12 %RH), or 0 if no reading.
 */
int32_t am2302_get_rh_x100(const am2302_ctx_t *ctx);

/**
 * @brief Return last valid temperature reading.
 * @return Temperature × 100 in °C (e.g. 2348 → 23.48 °C), or 0 if no reading.
 */
int32_t am2302_get_temp_x100(const am2302_ctx_t *ctx);

#else /* RIVR_FEATURE_AM2302 == 0 */

/* ── Stub types & functions (zero overhead when feature is disabled) ──────── */
typedef struct { int _dummy; } am2302_ctx_t;

static inline void    am2302_init(am2302_ctx_t *c, int g)        { (void)c; (void)g; }
static inline bool    am2302_tick(am2302_ctx_t *c, uint32_t t)   { (void)c; (void)t; return false; }
static inline int32_t am2302_get_rh_x100(const am2302_ctx_t *c)  { (void)c; return 0; }
static inline int32_t am2302_get_temp_x100(const am2302_ctx_t *c){ (void)c; return 0; }

#endif /* RIVR_FEATURE_AM2302 */
