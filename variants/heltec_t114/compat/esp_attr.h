/**
 * @file  esp_attr.h  (nRF52840 compat shim)
 * @brief Nullify ESP-IDF placement attributes on nRF52.
 *
 * ESP-IDF uses IRAM_ATTR to force code into IRAM (faster, ISR-safe).
 * On nRF52840 with Adafruit Arduino BSP all code runs from flash via XIP;
 * no separate IRAM section exists.  Map the attributes to empty / GCC equivalents.
 */

#ifndef RIVR_COMPAT_ESP_ATTR_H
#define RIVR_COMPAT_ESP_ATTR_H

/* Force function/variable into RAM — no separate IRAM on nRF52, use SRAM. */
#ifndef IRAM_ATTR
#  define IRAM_ATTR   __attribute__((section(".data")))
#endif

/* DRAM_ATTR: variables in DRAM — equivalent to normal .data/.bss on nRF52. */
#ifndef DRAM_ATTR
#  define DRAM_ATTR
#endif

/* RTC_NOINIT_ATTR: survives deep sleep on ESP32 — no equivalent on nRF52. */
#ifndef RTC_NOINIT_ATTR
#  define RTC_NOINIT_ATTR
#endif

/* RTC_DATA_ATTR / RTC_RODATA_ATTR: RTC slow memory — no equivalent on nRF52. */
#ifndef RTC_DATA_ATTR
#  define RTC_DATA_ATTR
#endif
#ifndef RTC_RODATA_ATTR
#  define RTC_RODATA_ATTR
#endif

#endif /* RIVR_COMPAT_ESP_ATTR_H */
