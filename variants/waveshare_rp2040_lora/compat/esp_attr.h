/**
 * @file  variants/waveshare_rp2040_lora/compat/esp_attr.h
 * @brief Nullify ESP-IDF placement attributes on RP2040.
 *
 * ESP-IDF uses IRAM_ATTR to force code into IRAM (faster, ISR-safe).
 * On RP2040 with arduino-pico all code runs from flash via XIP;
 * no separate IRAM section exists.  Map the attributes to empty / GCC equivalents.
 */

#ifndef RIVR_COMPAT_ESP_ATTR_H
#define RIVR_COMPAT_ESP_ATTR_H

/* Force function/variable into RAM — no separate IRAM on RP2040, use SRAM. */
#ifndef IRAM_ATTR
#  define IRAM_ATTR   __attribute__((section(".data")))
#endif

/* DRAM_ATTR: variables in DRAM — equivalent to normal .data/.bss on RP2040. */
#ifndef DRAM_ATTR
#  define DRAM_ATTR
#endif

/* RTC_NOINIT_ATTR: survives deep sleep on ESP32 — no equivalent on RP2040. */
#ifndef RTC_NOINIT_ATTR
#  define RTC_NOINIT_ATTR
#endif

/* RTC_DATA_ATTR / RTC_RODATA_ATTR: RTC slow memory — no equivalent on RP2040. */
#ifndef RTC_DATA_ATTR
#  define RTC_DATA_ATTR
#endif
#ifndef RTC_RODATA_ATTR
#  define RTC_RODATA_ATTR
#endif

#endif /* RIVR_COMPAT_ESP_ATTR_H */
