/**
 * @file  platform_esp32.c
 * @brief ESP32 GPIO/SPI hardware initialisation.
 */

#include "platform_esp32.h"
#include "radio_sx1262.h"   /* RF_MAX_PAYLOAD_LEN */

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"      /* esp_timer_get_time */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "PLATFORM"

spi_device_handle_t g_spi_sx1262;

void platform_init(void)
{
    ESP_LOGI(TAG, "platform_init: configuring GPIO and SPI");

    /* ── GPIO outputs ── */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_SX1262_NSS)   |
                        (1ULL << PIN_SX1262_RESET)  |
#if RIVR_RFSWITCH_ENABLE
                        (1ULL << PIN_SX1262_RXEN)   |
                        (1ULL << PIN_SX1262_TXEN)   |
#endif
                        (1ULL << PIN_LED_STATUS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    /* ── GPIO inputs ── */
    gpio_config_t in_cfg = {
        .pin_bit_mask =
#if !RIVR_RADIO_SX1276
                        (1ULL << PIN_SX1262_BUSY) |
#endif
                        (1ULL << PIN_SX1262_DIO1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,   /* DIO1: rising edge */
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* Start with NSS high, RESET high, LED off, antenna switch idle (both low) */
    gpio_set_level(PIN_SX1262_NSS,   1);
    gpio_set_level(PIN_SX1262_RESET, 1);
#if RIVR_RFSWITCH_ENABLE
    gpio_set_level(PIN_SX1262_RXEN,  0);
    gpio_set_level(PIN_SX1262_TXEN,  0);
#endif
    gpio_set_level(PIN_LED_STATUS,   0);

    /* ── VSPI bus ── */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = PIN_SX1262_MOSI,
        .miso_io_num   = PIN_SX1262_MISO,
        .sclk_io_num   = PIN_SX1262_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RF_MAX_PAYLOAD_LEN + 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SX1262_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* ── SX1262 device ── */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SX1262_SPI_FREQ_HZ,
        .mode           = 0,               /* CPOL=0, CPHA=0 */
        .spics_io_num   = -1,              /* Manual CS via platform_spi_cs_* */
        .queue_size     = 1,
        .flags          = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SX1262_SPI_HOST, &dev_cfg, &g_spi_sx1262));

    /* Install GPIO ISR service (isr_core_id=0 = same core as main) */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    ESP_LOGI(TAG, "platform_init: done");
}

/* ── SPI helpers ─────────────────────────────────────────────────────────── */

void platform_spi_cs_assert(void)  { gpio_set_level(PIN_SX1262_NSS, 0); }
void platform_spi_cs_release(void) { gpio_set_level(PIN_SX1262_NSS, 1); }

void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    /* Use a stack-local transaction descriptor (no heap) */
    spi_transaction_t t = {
        .length    = (size_t)len * 8,  /* bits */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_acquire_bus(g_spi_sx1262, portMAX_DELAY);
    platform_spi_cs_assert();
    spi_device_transmit(g_spi_sx1262, &t);
    platform_spi_cs_release();
    spi_device_release_bus(g_spi_sx1262);
}

bool platform_sx1262_wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (gpio_get_level(PIN_SX1262_BUSY)) {
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        if (elapsed > timeout_ms) {
            ESP_LOGE("PLATFORM", "SX1262 BUSY timeout (%u ms)", timeout_ms);
            return false;
        }
    }
    return true;
}

void platform_sx1262_reset(void)
{
    gpio_set_level(PIN_SX1262_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PIN_SX1262_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ── Antenna switch ────────────────────────────────────────────────── */
/**
 * Drive the RF-switch control lines.
 *   enable=true  → RX path: RXEN=1, TXEN=0
 *   enable=false → TX path: RXEN=0, TXEN=1
 *
 * TXEN is also wired to SX1262 DIO2 on some boards (SetDio2AsRfSwitchCtrl),
 * but driving it explicitly from GPIO13 is always correct and covers boards
 * that do not have the DIO2→TXEN trace.  Both drivers agree on the level so
 * there is no bus conflict.
 */
void platform_sx1262_set_rxen(bool enable)
{
#if RIVR_RFSWITCH_ENABLE
    gpio_set_level(PIN_SX1262_RXEN, enable ? 1 : 0);
    gpio_set_level(PIN_SX1262_TXEN, enable ? 0 : 1);
#else
    (void)enable;   /* SX1276 / boards without external RF switch */
#endif
}

/* ── LED ─────────────────────────────────────────────────────────────────── */
void platform_led_on(void)     { gpio_set_level(PIN_LED_STATUS, 1); }
void platform_led_off(void)    { gpio_set_level(PIN_LED_STATUS, 0); }
void platform_led_toggle(void)
{
    static uint8_t state = 0;
    state ^= 1;
    gpio_set_level(PIN_LED_STATUS, state);
}

/* ── Delay ───────────────────────────────────────────────────────────────── */
void platform_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}
