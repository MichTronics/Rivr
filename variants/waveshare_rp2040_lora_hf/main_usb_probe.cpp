/**
 * @file  variants/waveshare_rp2040_lora_hf/main_usb_probe.cpp
 * @brief Minimal RP2040 USB probe image for Waveshare RP2040-LoRa-HF.
 *
 * This intentionally skips all Rivr and radio startup. It exists only to
 * verify that the board boots, the LED can be controlled, and USB CDC
 * enumerates cleanly.
 */

#include <Arduino.h>

#ifndef PIN_LED_STATUS
#define PIN_LED_STATUS 25
#endif

static constexpr uint32_t kHeartbeatMs = 1000u;
static uint32_t s_last_beat_ms = 0u;
static uint32_t s_counter = 0u;

void setup(void)
{
    pinMode(PIN_LED_STATUS, OUTPUT);
    digitalWrite(PIN_LED_STATUS, LOW);

    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("RIVR RP2040 USB probe boot");
    Serial.println("LED forced off, radio disabled");
}

void loop(void)
{
    uint32_t now = millis();
    if ((uint32_t)(now - s_last_beat_ms) >= kHeartbeatMs) {
        s_last_beat_ms = now;
        Serial.printf("probe alive %lu ms #%lu\r\n",
                      (unsigned long)now,
                      (unsigned long)++s_counter);
    }
    delay(10);
}
