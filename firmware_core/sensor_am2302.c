/**
 * @file  sensor_am2302.c
 * @brief AM2302 (DHT22) single-bus humidity + temperature driver (ESP32).
 *
 * Protocol summary (from AM2302 datasheet):
 *   1. Host pulls bus low for ≥ 1 ms (start signal), then releases.
 *   2. Device responds: ~80 µs low, ~80 µs high.
 *   3. Device transmits 40 bits: each bit starts with ~50 µs low, then:
 *        high ≤ 30 µs → '0'
 *        high ≥ 70 µs → '1'
 *   4. Last bit followed by ~50 µs low (bus idle).
 *
 * Byte order: RH_high, RH_low, T_high, T_low, Checksum.
 * RH = ((RH_high << 8) | RH_low) * 0.1  → stored as ×100 (×1000 / 10)
 * T  = ((T_high & 0x7F) << 8) | T_low)  * 0.1  (sign bit in T_high[7])
 *
 * Interrupts are disabled for the ~5 ms data-read window only. This is the
 * minimum necessary for reliable bit timing on a non-RTOS pin.
 */

#include "sensor_am2302.h"

#if RIVR_FEATURE_AM2302

#include "driver/gpio.h"
#include "esp_rom_sys.h"        /* esp_rom_delay_us()       */
#include "esp_timer.h"          /* esp_timer_get_time()     */
#include "freertos/FreeRTOS.h"  /* portDISABLE_INTERRUPTS() */
#include "esp_log.h"
#include <string.h>

#define TAG "AM2302"

/* ── Timing constants (µs) ─────────────────────────────────────────────────*/
#define AM_START_LOW_US     1100u  /**< Start signal: pull low for 1.1 ms  */
#define AM_RELEASE_US         55u  /**< Release wait before sampling response */
#define AM_RESP_TIMEOUT_US   200u  /**< Max wait for device response edge   */
#define AM_BIT_THRESHOLD_US   50u  /**< High-pulse > this → bit = '1'      */
#define AM_BIT_TIMEOUT_US    150u  /**< Abort if stuck high this long      */

/* ── Helpers ────────────────────────────────────────────────────────────── */

/**
 * Busy-wait for the GPIO to reach 'expect_level', up to 'timeout_us'.
 * Uses esp_timer_get_time() for real µs measurement — immune to loop overhead.
 * MUST be called with interrupts already disabled.
 */
static int64_t wait_level(int gpio, int expect_level, int64_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio) != expect_level) {
        if ((esp_timer_get_time() - start) >= timeout_us) return -1;
    }
    return esp_timer_get_time() - start;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void am2302_init(am2302_ctx_t *ctx, int gpio)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->gpio = gpio;
    /* Place last_read_ms far enough in the past to allow an immediate first
     * read request (will still wait until first am2302_tick() call, which
     * checks the interval). We want first read to happen at the normal
     * scheduled interval in sensors.c, not at t=0. */
    ctx->last_read_ms = 0u;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    /* ~45 kΩ internal; add external 10 kΩ for reliable edges */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(gpio, 1);   /* release bus, pull-up idles it HIGH */

    ESP_LOGI(TAG, "init GPIO%d", gpio);
}

bool am2302_tick(am2302_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return false;

    /* Enforce minimum interval */
    if (ctx->last_read_ms != 0u &&
        (now_ms - ctx->last_read_ms) < RIVR_AM2302_MIN_INTERVAL_MS) {
        return false;
    }
    ctx->last_read_ms = now_ms;

    /* ── Start signal: host drives bus low for 1.1 ms ─────────────────── */
    gpio_set_level(ctx->gpio, 0);
    esp_rom_delay_us(AM_START_LOW_US);

    /* ── Release and wait for device to respond ────────────────────────── */
    /* Disable interrupts BEFORE releasing so the full response + data window
     * (~5 ms) is uninterrupted.  Matches Adafruit DHT library behaviour.  */
    portDISABLE_INTERRUPTS();
    gpio_set_level(ctx->gpio, 1);   /* end start signal */
    esp_rom_delay_us(AM_RELEASE_US); /* 55 µs: device pulls low within ~20-40 µs */

    /* Expect device to pull bus low (response low) */
    if (wait_level(ctx->gpio, 0, AM_RESP_TIMEOUT_US) < 0) goto error;
    /* Then device releases (response high) */
    if (wait_level(ctx->gpio, 1, AM_RESP_TIMEOUT_US) < 0) goto error;
    /* Then device pulls low again to start data */
    if (wait_level(ctx->gpio, 0, AM_RESP_TIMEOUT_US) < 0) goto error;

    /* ── Read 40 bits ──────────────────────────────────────────────────── */
    uint8_t data[5];
    memset(data, 0, sizeof(data));

    for (int bit = 39; bit >= 0; bit--) {
        /* Wait for rising edge (end of ~50 µs low pre-pulse) */
        if (wait_level(ctx->gpio, 1, AM_RESP_TIMEOUT_US) < 0) goto error;

        /* Measure how long the line stays high using real µs timestamps.
         * A '0' bit stays high ~26 µs; a '1' bit stays high ~70 µs.
         * Threshold = 50 µs.  This is immune to loop/call overhead. */
        int64_t t_start = esp_timer_get_time();
        while (gpio_get_level(ctx->gpio) == 1) {
            if ((esp_timer_get_time() - t_start) > AM_BIT_TIMEOUT_US) goto error;
        }
        int64_t high_us = esp_timer_get_time() - t_start;

        if (high_us > AM_BIT_THRESHOLD_US) {
            data[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
    }

    portENABLE_INTERRUPTS();

    /* ── Validate checksum ─────────────────────────────────────────────── */
    /* Byte order stored by the reverse loop (bit 39 first = received first):
     *   data[4] = RH_high, data[3] = RH_low,
     *   data[2] = T_high,  data[1] = T_low, data[0] = Checksum            */
    uint8_t checksum = (uint8_t)((uint16_t)data[4] + data[3] + data[2] + data[1]);
    if (checksum != data[0]) {
        ESP_LOGW(TAG, "checksum error (calc=%02X recv=%02X)", checksum, data[0]);
        return false;
    }

    /* ── Decode values ─────────────────────────────────────────────────── */
    /* AM2302 transmits MSB first, byte order: RH_hi, RH_lo, T_hi, T_lo, CRC.
     * The reverse-index loop above stores: data[4]=RH_hi, data[3]=RH_lo,
     * data[2]=T_hi, data[1]=T_lo, data[0]=CRC.                            */
    uint16_t rh_raw   = ((uint16_t)data[4] << 8) | data[3];
    uint16_t t_raw    = ((uint16_t)(data[2] & 0x7Fu) << 8) | data[1];
    bool     negative = (data[2] & 0x80u) != 0;

    /* raw unit = 0.1 units, we want ×100: multiply by 10 */
    ctx->rh_x100   = (int32_t)rh_raw * 10;
    ctx->temp_x100 = (int32_t)t_raw  * 10;
    if (negative) ctx->temp_x100 = -ctx->temp_x100;
    ctx->valid = true;

    ESP_LOGI(TAG, "RH=%ld.%02ld%% T=%ld.%02ld°C",
             (long)(ctx->rh_x100   / 100), (long)(ctx->rh_x100   % 100),
             (long)(ctx->temp_x100 / 100), (long)((ctx->temp_x100 < 0 ? -ctx->temp_x100 : ctx->temp_x100) % 100));
    return true;

error:
    portENABLE_INTERRUPTS();
    gpio_set_level(ctx->gpio, 1);   /* make sure bus is released */
    ESP_LOGW(TAG, "read error");
    return false;
}

int32_t am2302_get_rh_x100(const am2302_ctx_t *ctx)
{
    return (ctx && ctx->valid) ? ctx->rh_x100 : 0;
}

int32_t am2302_get_temp_x100(const am2302_ctx_t *ctx)
{
    return (ctx && ctx->valid) ? ctx->temp_x100 : 0;
}

#endif /* RIVR_FEATURE_AM2302 */
