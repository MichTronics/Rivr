/**
 * @file  sensor_ds18b20.h
 * @brief DS18B20 1-Wire temperature sensor — non-blocking state machine.
 *
 * Enabled when RIVR_FEATURE_DS18B20=1.  When disabled this header compiles
 * to empty structs and inline no-ops so callers need no #if guards.
 *
 * Usage (in main loop):
 *   ds18b20_ctx_t ctx;
 *   ds18b20_init(&ctx, PIN_DS18B20_ONEWIRE);       // call once at boot
 *
 *   // Once per loop:
 *   ds18b20_tick(&ctx, now_ms);
 *
 *   // When ds18b20_ready() returns true, consume the reading:
 *   int32_t t = ds18b20_read_celsius_x100(&ctx);   // °C × 100
 *
 * Hardware:
 *   DS18B20 DQ pin → GPIO with 4.7 kΩ pull-up to 3.3 V (external).
 *   Recommended ESP32 DevKit + E22: GPIO27 (free, no conflicts).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if RIVR_FEATURE_DS18B20

/** DS18B20 12-bit conversion time per datasheet is 750 ms max.
 *  Add 50 ms safety margin to absorb clock drift. */
#define DS18B20_CONV_TIME_MS   800u

typedef enum {
    DS18B20_STATE_IDLE       = 0,  /**< Waiting for next poll cycle           */
    DS18B20_STATE_CONVERTING = 1,  /**< Conversion in progress (800 ms wait) */
    DS18B20_STATE_READY      = 2,  /**< Reading done, result available        */
    DS18B20_STATE_ERROR      = 3,  /**< No device found or CRC fail           */
} ds18b20_state_t;

typedef struct {
    int              gpio;               /**< OneWire data pin (open-drain)    */
    ds18b20_state_t  state;
    uint32_t         convert_start_ms;
    int32_t          celsius_x100;       /**< Last valid reading (°C × 100)   */
    bool             valid;              /**< true after first successful read */
} ds18b20_ctx_t;

/**
 * @brief Initialise the driver and configure the GPIO as open-drain.
 * @param ctx   Caller-allocated context (zero-init on entry).
 * @param gpio  GPIO number of the DQ pin.
 */
void ds18b20_init(ds18b20_ctx_t *ctx, int gpio);

/**
 * @brief Kick off a temperature conversion.
 *
 * Issues a 1-Wire reset + Skip ROM + Convert-T command (~2 ms blocking).
 * Call this when the poll interval has elapsed and the state is IDLE/ERROR.
 *
 * @return true if the conversion was started (device present), false otherwise.
 */
bool ds18b20_start_conversion(ds18b20_ctx_t *ctx, uint32_t now_ms);

/**
 * @brief Advance the state machine — call once per main-loop iteration.
 *
 * When the conversion window has elapsed (CONVERTING → READY path),
 * reads the scratchpad, verifies CRC, and stores the result.
 */
void ds18b20_tick(ds18b20_ctx_t *ctx, uint32_t now_ms);

/** @return true when a fresh reading has been stored by ds18b20_tick(). */
bool ds18b20_ready(const ds18b20_ctx_t *ctx);

/**
 * @brief Return the latest temperature reading and reset the ready flag.
 * @return Temperature in °C × 100 (e.g. 2350 = 23.50 °C).
 */
int32_t ds18b20_read_celsius_x100(ds18b20_ctx_t *ctx);

#else  /* !RIVR_FEATURE_DS18B20 — stub everything out */

typedef struct { int _dummy; } ds18b20_ctx_t;
static inline void    ds18b20_init(ds18b20_ctx_t *c, int g)              { (void)c; (void)g; }
static inline bool    ds18b20_start_conversion(ds18b20_ctx_t *c, uint32_t t) { (void)c; (void)t; return false; }
static inline void    ds18b20_tick(ds18b20_ctx_t *c, uint32_t t)         { (void)c; (void)t; }
static inline bool    ds18b20_ready(const ds18b20_ctx_t *c)              { (void)c; return false; }
static inline int32_t ds18b20_read_celsius_x100(ds18b20_ctx_t *c)        { (void)c; return 0; }

#endif /* RIVR_FEATURE_DS18B20 */

#ifdef __cplusplus
}
#endif
