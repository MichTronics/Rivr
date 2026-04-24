/**
 * @file  sensor_ds18b20.c
 * @brief DS18B20 1-Wire temperature sensor — non-blocking driver (ESP32).
 *
 * Bit-bang 1-Wire implementation using open-drain GPIO.
 * Interrupts are disabled only for the narrow timing windows within each
 * bit operation (≤ 70 µs per bit), making the driver safe to call from
 * the main loop without disturbing LoRa RX timing.
 *
 * State machine flow per measurement cycle:
 *   ds18b20_start_conversion()  →  issues reset + SkipROM + ConvertT (~2 ms)
 *                                   sets state = CONVERTING
 *   ds18b20_tick() (after 800 ms) → reads 9-byte scratchpad, verifies CRC
 *                                   sets state = READY or ERROR
 *   ds18b20_read_celsius_x100()  →  returns result, clears READY → IDLE
 */

#include "sensor_ds18b20.h"

#if RIVR_FEATURE_DS18B20

#include "driver/gpio.h"
#include "esp_rom_sys.h"        /* esp_rom_delay_us()         */
#include "freertos/FreeRTOS.h"  /* portDISABLE_INTERRUPTS()   */
#include "esp_log.h"
#include <string.h>

#define TAG "DS18B20"

/* ── 1-Wire timing constants (µs) per Maxim Application Note 126 ─────────── */
#define OW_RESET_LOW_US     480u   /**< Reset pulse low time                 */
#define OW_RESET_WAIT_US     70u   /**< Time before sampling presence pulse  */
#define OW_RESET_TAIL_US    410u   /**< Remainder of reset window            */
#define OW_WRITE1_LOW_US      5u   /**< Write-1: short low pulse             */
#define OW_WRITE0_LOW_US     60u   /**< Write-0: long low pulse              */
#define OW_SLOT_US           65u   /**< Total slot duration                  */
#define OW_READ_HOLD_US       3u   /**< Read slot: initial low duration      */
#define OW_READ_SAMPLE_US     7u   /**< Read slot: time from release to sample */
#define OW_READ_TAIL_US      55u   /**< Read slot: guard time after sample   */

/* 1-Wire ROM commands */
#define OW_CMD_SKIP_ROM     0xCCu
#define OW_CMD_CONVERT_T    0x44u
#define OW_CMD_READ_SCRATCH  0xBEu
#define OW_CMD_WRITE_SCRATCH 0x4Eu

/* Configuration register bits[6:5] select resolution:
 *   0x1F = 9-bit  (0.5°C   steps, ~94 ms)   ← common EEPROM default
 *   0x3F = 10-bit (0.25°C  steps, ~188 ms)
 *   0x5F = 11-bit (0.125°C steps, ~375 ms)
 *   0x7F = 12-bit (0.0625°C steps, ~750 ms) ← driver decode assumes this
 */
#define DS18B20_CFG_12BIT    0x7Fu

/* ── Low-level 1-Wire primitives ─────────────────────────────────────────── */

/**
 * Pin is configured as INPUT_OUTPUT_OD (open-drain) at init.
 *   gpio_set_level(gpio, 0) → actively pulls bus LOW
 *   gpio_set_level(gpio, 1) → releases bus (external pull-up takes it HIGH)
 */

/** Issue reset pulse and detect presence.  Interrupts briefly disabled for
 *  the 70 µs presence-detection window only. */
static bool ow_reset(int gpio)
{
    /* Drive bus low for reset pulse — timing not critical, interrupts OK */
    gpio_set_level(gpio, 0);
    esp_rom_delay_us(OW_RESET_LOW_US);

    /* Release and sample presence pulse — brief critical window */
    portDISABLE_INTERRUPTS();
    gpio_set_level(gpio, 1);
    esp_rom_delay_us(OW_RESET_WAIT_US);
    int presence = gpio_get_level(gpio);  /* 0 if device pulled bus low */
    portENABLE_INTERRUPTS();

    esp_rom_delay_us(OW_RESET_TAIL_US);   /* Wait out remainder of slot */
    return (presence == 0);               /* true → device present      */
}

/** Write one bit.  Interrupts disabled for the ≤65 µs slot window. */
static void ow_write_bit(int gpio, uint8_t bit)
{
    portDISABLE_INTERRUPTS();
    gpio_set_level(gpio, 0);                    /* start time-slot    */
    if (bit) {
        esp_rom_delay_us(OW_WRITE1_LOW_US);
        gpio_set_level(gpio, 1);                /* release early → '1' */
        portENABLE_INTERRUPTS();
        esp_rom_delay_us(OW_SLOT_US - OW_WRITE1_LOW_US);
    } else {
        esp_rom_delay_us(OW_WRITE0_LOW_US);
        gpio_set_level(gpio, 1);                /* release late → '0' */
        portENABLE_INTERRUPTS();
        esp_rom_delay_us(OW_SLOT_US - OW_WRITE0_LOW_US);
    }
}

/** Write one byte LSB-first. */
static void ow_write_byte(int gpio, uint8_t byte)
{
    for (uint8_t i = 0; i < 8u; i++) {
        ow_write_bit(gpio, (byte >> i) & 0x01u);
    }
}

/** Read one bit.  Interrupts disabled across the 10 µs sample window. */
static uint8_t ow_read_bit(int gpio)
{
    portDISABLE_INTERRUPTS();
    gpio_set_level(gpio, 0);                    /* initiate read slot */
    esp_rom_delay_us(OW_READ_HOLD_US);
    gpio_set_level(gpio, 1);                    /* release            */
    esp_rom_delay_us(OW_READ_SAMPLE_US);
    uint8_t bit = (uint8_t)gpio_get_level(gpio);
    portENABLE_INTERRUPTS();
    esp_rom_delay_us(OW_READ_TAIL_US);          /* guard time         */
    return bit;
}

/** Read one byte LSB-first. */
static uint8_t ow_read_byte(int gpio)
{
    uint8_t byte = 0;
    for (uint8_t i = 0; i < 8u; i++) {
        byte |= (uint8_t)(ow_read_bit(gpio) << i);
    }
    return byte;
}

/* ── CRC-8 (Dallas/Maxim, poly 0x8C reflected) ───────────────────────────── */
static uint8_t ow_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (uint8_t j = 0; j < 8u; j++) {
            uint8_t mix = (crc ^ byte) & 0x01u;
            crc >>= 1;
            if (mix) crc ^= 0x8Cu;
            byte >>= 1;
        }
    }
    return crc;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ds18b20_init(ds18b20_ctx_t *ctx, int gpio)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->gpio  = gpio;
    ctx->state = DS18B20_STATE_IDLE;

    /* Configure pin as open-drain — set level to 1 (bus idle/high) */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_DISABLE,    /* external 4.7 kΩ required */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(gpio, 1);   /* release bus */

    /* Force 12-bit resolution via Write Scratchpad.
     * Without this the sensor uses its EEPROM default — often 9-bit (0.5°C
     * steps) — which causes a blocky staircase in charts.  The driver decode
     * formula (raw * 25 / 4) assumes 12-bit; setting it here makes them match.
     * Conversion time at 12-bit is ~750 ms; the 800 ms wait in ds18b20_tick()
     * already covers this. */
    if (ow_reset(gpio)) {
        ow_write_byte(gpio, OW_CMD_SKIP_ROM);
        ow_write_byte(gpio, OW_CMD_WRITE_SCRATCH);
        ow_write_byte(gpio, 0x00u);           /* TH alarm — not used */
        ow_write_byte(gpio, 0x00u);           /* TL alarm — not used */
        ow_write_byte(gpio, DS18B20_CFG_12BIT);
        ESP_LOGI(TAG, "init GPIO%d — resolution set to 12-bit", gpio);
    } else {
        ESP_LOGW(TAG, "init GPIO%d — no device detected", gpio);
    }
}

bool ds18b20_start_conversion(ds18b20_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return false;
    if (ctx->state == DS18B20_STATE_CONVERTING) return false; /* already running */

    /* Reset + check presence */
    if (!ow_reset(ctx->gpio)) {
        ESP_LOGW(TAG, "no device on GPIO%d", ctx->gpio);
        ctx->state = DS18B20_STATE_ERROR;
        return false;
    }

    /* Skip ROM (broadcast to all devices on bus) + start conversion */
    ow_write_byte(ctx->gpio, OW_CMD_SKIP_ROM);
    ow_write_byte(ctx->gpio, OW_CMD_CONVERT_T);

    ctx->convert_start_ms = now_ms;
    ctx->state            = DS18B20_STATE_CONVERTING;
    ESP_LOGD(TAG, "conversion started at %lu ms", (unsigned long)now_ms);
    return true;
}

void ds18b20_tick(ds18b20_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx || ctx->state != DS18B20_STATE_CONVERTING) return;
    if ((now_ms - ctx->convert_start_ms) < DS18B20_CONV_TIME_MS) return;

    /* Read scratchpad (9 bytes): temp_LSB, temp_MSB, TH, TL, cfg, 3×FFh, CRC */
    if (!ow_reset(ctx->gpio)) {
        ESP_LOGW(TAG, "device lost after conversion");
        ctx->state = DS18B20_STATE_ERROR;
        return;
    }
    ow_write_byte(ctx->gpio, OW_CMD_SKIP_ROM);
    ow_write_byte(ctx->gpio, OW_CMD_READ_SCRATCH);

    uint8_t scratch[9];
    for (uint8_t i = 0; i < 9u; i++) {
        scratch[i] = ow_read_byte(ctx->gpio);
    }

    /* Verify CRC over bytes 0–7, expected in byte 8 */
    if (ow_crc8(scratch, 8u) != scratch[8]) {
        ESP_LOGW(TAG, "CRC error");
        ctx->state = DS18B20_STATE_ERROR;
        return;
    }

    /* Decode temperature: 16-bit two's complement, LSB = 1/16 °C (12-bit) */
    int16_t raw = (int16_t)((uint16_t)scratch[0] | ((uint16_t)scratch[1] << 8));
    /* raw × 100 / 16 = raw × 25 / 4 — no floating point */
    ctx->celsius_x100 = ((int32_t)raw * 25) / 4;
    ctx->valid        = true;
    ctx->state        = DS18B20_STATE_READY;

    ESP_LOGI(TAG, "temp = %ld.%02ld °C",
             (long)(ctx->celsius_x100 / 100),
             (long)((ctx->celsius_x100 < 0 ? -ctx->celsius_x100 : ctx->celsius_x100) % 100));
}

bool ds18b20_ready(const ds18b20_ctx_t *ctx)
{
    return ctx && (ctx->state == DS18B20_STATE_READY);
}

int32_t ds18b20_read_celsius_x100(ds18b20_ctx_t *ctx)
{
    if (!ctx) return 0;
    ctx->state = DS18B20_STATE_IDLE;  /* consume reading, return to idle */
    return ctx->celsius_x100;
}

#endif /* RIVR_FEATURE_DS18B20 */
