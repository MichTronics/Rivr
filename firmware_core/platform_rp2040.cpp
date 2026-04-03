/**
 * @file  firmware_core/platform_rp2040.cpp
 * @brief RP2040 platform HAL implementation (Arduino + Pico SDK).
 *
 * SPI1 and the SX1262 control pins follow the same mapping used by:
 * - Meshtastic `variants/rp2040/rp2040-lora/variant.h`
 * - MeshCore `variants/waveshare_rp2040_lora`
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "platform_rp2040.h"

#include <Arduino.h>
#include <SPI.h>
#include <hardware/watchdog.h>
#include "nvs_flash.h"

#define TAG "PLATFORM"
#define SX1262_SPI_FREQ_HZ 2000000UL

static SPIClassRP2040 &s_lora_spi = SPI1;

void platform_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    pinMode(PIN_SX1262_NSS, OUTPUT);
    pinMode(PIN_SX1262_RESET, OUTPUT);
    pinMode(PIN_LED_STATUS, OUTPUT);

#if RIVR_RFSWITCH_ENABLE
    pinMode(PIN_SX1262_RXEN, OUTPUT);
    digitalWrite(PIN_SX1262_RXEN, LOW);
#endif

    digitalWrite(PIN_SX1262_NSS, HIGH);
    digitalWrite(PIN_SX1262_RESET, HIGH);
    digitalWrite(PIN_LED_STATUS, LOW);

    pinMode(PIN_SX1262_BUSY, INPUT);
    pinMode(PIN_SX1262_DIO1, INPUT);

    s_lora_spi.setSCK(PIN_SX1262_SCK);
    s_lora_spi.setTX(PIN_SX1262_MOSI);
    s_lora_spi.setRX(PIN_SX1262_MISO);
    s_lora_spi.begin(false);

    Serial.printf("[I][%s] LittleFS-backed NVS initialised\r\n", TAG);
    Serial.printf("[I][%s] platform_init done (RP2040)\r\n", TAG);
}

void platform_spi_cs_assert(void)
{
    digitalWrite(PIN_SX1262_NSS, LOW);
}

void platform_spi_cs_release(void)
{
    digitalWrite(PIN_SX1262_NSS, HIGH);
}

void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    platform_spi_begin();
    platform_spi_transfer_raw(tx, rx, len);
    platform_spi_end();
}

void platform_spi_begin(void)
{
    s_lora_spi.beginTransaction(SPISettings(SX1262_SPI_FREQ_HZ, MSBFIRST, SPI_MODE0));
    platform_spi_cs_assert();
}

void platform_spi_transfer_raw(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint8_t out = tx ? tx[i] : 0x00u;
        uint8_t in = s_lora_spi.transfer(out);
        if (rx) {
            rx[i] = in;
        }
    }
}

void platform_spi_end(void)
{
    platform_spi_cs_release();
    s_lora_spi.endTransaction();
}

bool platform_sx1262_wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (digitalRead(PIN_SX1262_BUSY)) {
        if ((millis() - t0) > timeout_ms) {
            Serial.printf("[E][%s] SX1262 BUSY timeout (%lu ms)\r\n",
                          TAG,
                          (unsigned long)timeout_ms);
            return false;
        }
    }
    return true;
}

void platform_sx1262_reset(void)
{
    digitalWrite(PIN_SX1262_RESET, LOW);
    delay(1);
    digitalWrite(PIN_SX1262_RESET, HIGH);
    delay(10);
}

void platform_sx1262_set_rxen(bool enable)
{
#if RIVR_RFSWITCH_ENABLE
    /* Waveshare follows the Meshtastic/MeshCore mapping:
     * DIO2 drives the RF switch control during TX, GPIO17 drives the
     * complementary receive-enable path. */
    digitalWrite(PIN_SX1262_RXEN, enable ? HIGH : LOW);
#else
    (void)enable;
#endif
}

void platform_dio1_attach_isr(void (*isr)(void))
{
    attachInterrupt(digitalPinToInterrupt(PIN_SX1262_DIO1), isr, RISING);
}

void platform_led_on(void)  { digitalWrite(PIN_LED_STATUS, HIGH); }
void platform_led_off(void) { digitalWrite(PIN_LED_STATUS, LOW); }

void platform_led_toggle(void)
{
    static uint8_t s = 0u;
    s ^= 1u;
    digitalWrite(PIN_LED_STATUS, s ? HIGH : LOW);
}

uint32_t platform_millis(void)
{
    return millis();
}

void platform_restart(void)
{
    watchdog_reboot(0, 0, 0);
    while (1) {
        tight_loop_contents();
    }
}

#endif /* RIVR_PLATFORM_RP2040 */
