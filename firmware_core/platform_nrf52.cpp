/**
 * @file  firmware_core/platform_nrf52.c
 * @brief nRF52840 platform HAL implementation (Arduino framework).
 *
 * Uses the Arduino SPI and GPIO APIs supplied by the Adafruit nRF52 BSP.
 * The Adafruit BSP bundles FreeRTOS so vTaskDelay() and friends are
 * available in the same build without adding extra dependencies.
 *
 * Only compiled when RIVR_PLATFORM_NRF52840 is defined (set by config.h).
 */

#if defined(RIVR_PLATFORM_NRF52840)

#include "platform_nrf52.h"
#include <Arduino.h>
#include <SPI.h>

#define TAG "PLATFORM"

/* ── SPI settings ─────────────────────────────────────────────────────────── */
#define SX1262_SPI_FREQ_HZ  8000000UL

void platform_init(void)
{
    /* ── GPIO outputs ── */
    pinMode(PIN_SX1262_NSS,   OUTPUT);
    pinMode(PIN_SX1262_RESET, OUTPUT);
    pinMode(PIN_LED_STATUS,   OUTPUT);

#if RIVR_RFSWITCH_ENABLE
    pinMode(PIN_SX1262_RXEN,  OUTPUT);
    pinMode(PIN_SX1262_TXEN,  OUTPUT);
    digitalWrite(PIN_SX1262_RXEN, LOW);
    digitalWrite(PIN_SX1262_TXEN, LOW);
#endif

    /* Safe initial state */
    digitalWrite(PIN_SX1262_NSS,   HIGH);
    digitalWrite(PIN_SX1262_RESET, HIGH);
    digitalWrite(PIN_LED_STATUS,   LOW);

    /* ── GPIO inputs ── */
    pinMode(PIN_SX1262_BUSY, INPUT);
    pinMode(PIN_SX1262_DIO1, INPUT);

    /* ── SPI bus ── */
    SPI.begin();

    Serial.printf("[I][%s] platform_init done (nRF52840)\r\n", TAG);
}

/* ── SPI helpers ─────────────────────────────────────────────────────────── */

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
    SPI.beginTransaction(SPISettings(SX1262_SPI_FREQ_HZ, MSBFIRST, SPI_MODE0));
    platform_spi_cs_assert();
    for (uint16_t i = 0; i < len; i++) {
        rx[i] = SPI.transfer(tx ? tx[i] : 0x00u);
    }
    platform_spi_cs_release();
    SPI.endTransaction();
}

bool platform_sx1262_wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (digitalRead(PIN_SX1262_BUSY)) {
        if ((millis() - t0) > timeout_ms) {
            Serial.printf("[E][%s] SX1262 BUSY timeout (%u ms)\r\n", TAG, timeout_ms);
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

/* ── Antenna switch ───────────────────────────────────────────────────────── */

void platform_sx1262_set_rxen(bool enable)
{
#if RIVR_RFSWITCH_ENABLE
    digitalWrite(PIN_SX1262_RXEN, enable ? HIGH : LOW);
    digitalWrite(PIN_SX1262_TXEN, enable ? LOW  : HIGH);
#else
    (void)enable;
#endif
}

/* ── GPIO ISR ─────────────────────────────────────────────────────────────── */

void platform_dio1_attach_isr(void (*isr)(void))
{
    attachInterrupt(digitalPinToInterrupt(PIN_SX1262_DIO1), isr, RISING);
}

/* ── LED ──────────────────────────────────────────────────────────────────── */

void platform_led_on(void)      { digitalWrite(PIN_LED_STATUS, HIGH); }
void platform_led_off(void)     { digitalWrite(PIN_LED_STATUS, LOW);  }
void platform_led_toggle(void)
{
    static uint8_t s = 0;
    s ^= 1u;
    digitalWrite(PIN_LED_STATUS, s);
}

/* ── Misc ─────────────────────────────────────────────────────────────────── */

uint32_t platform_millis(void)  { return millis(); }
void platform_restart(void)     { NVIC_SystemReset(); }

#endif /* RIVR_PLATFORM_NRF52840 */
