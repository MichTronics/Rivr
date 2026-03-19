/**
 * @file  firmware_core/platform_rp2040.cpp
 * @brief RP2040 platform HAL implementation (Arduino + arduino-pico framework).
 *
 * Uses the Arduino SPI and GPIO APIs supplied by the earlephilhower
 * arduino-pico BSP.  The Waveshare RP2040 LoRa board wires the SX1262 to
 * SPI1 (not SPI0), so SPI1 is initialised with explicit pin assignments.
 *
 * Only compiled when RIVR_PLATFORM_RP2040 is defined (set by config.h).
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "platform_rp2040.h"
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
#if defined(PIN_SX1262_ANT_SW)
    pinMode(PIN_SX1262_ANT_SW, OUTPUT);
#endif

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
#if defined(PIN_SX1262_ANT_SW)
    digitalWrite(PIN_SX1262_ANT_SW, HIGH);
#endif

    /* ── GPIO inputs ── */
    pinMode(PIN_SX1262_BUSY, INPUT);
    pinMode(PIN_SX1262_DIO1, INPUT);

    /* ── SPI1 bus — Waveshare RP2040 LoRa uses SPI1 ── */
    SPI1.setRX(PIN_SX1262_MISO);
    SPI1.setTX(PIN_SX1262_MOSI);
    SPI1.setSCK(PIN_SX1262_SCK);
    SPI1.begin();

    Serial.printf("[I][%s] platform_init done (RP2040)\r\n", TAG);
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

void platform_spi_begin(void)
{
    SPI1.beginTransaction(SPISettings(SX1262_SPI_FREQ_HZ, MSBFIRST, SPI_MODE0));
    platform_spi_cs_assert();
}

void platform_spi_end(void)
{
    platform_spi_cs_release();
    SPI1.endTransaction();
}

void platform_spi_write_read_raw(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = SPI1.transfer(tx ? tx[i] : 0x00u);
        if (rx) rx[i] = b;
    }
}

void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    platform_spi_begin();
    platform_spi_write_read_raw(tx, rx, len);
    platform_spi_end();
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
#if defined(PIN_SX1262_ANT_SW)
    /* Waveshare RP2040 LoRa uses a single RF switch GPIO:
     * HIGH selects RX/idle, LOW selects TX. */
    digitalWrite(PIN_SX1262_ANT_SW, enable ? HIGH : LOW);
#elif RIVR_RFSWITCH_ENABLE
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

void platform_restart(void)
{
    /* arduino-pico provides rp2040.reboot() */
    rp2040.reboot();
}

#endif /* RIVR_PLATFORM_RP2040 */
