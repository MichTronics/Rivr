/**
 * @file  esp_attr.h  (RP2040 compat shim)
 * @brief Nullify ESP-IDF placement attributes on RP2040.
 */

#ifndef RIVR_COMPAT_ESP_ATTR_H
#define RIVR_COMPAT_ESP_ATTR_H

#ifndef IRAM_ATTR
#  define IRAM_ATTR __attribute__((section(".data")))
#endif
#ifndef DRAM_ATTR
#  define DRAM_ATTR
#endif
#ifndef RTC_NOINIT_ATTR
#  define RTC_NOINIT_ATTR
#endif
#ifndef RTC_DATA_ATTR
#  define RTC_DATA_ATTR
#endif
#ifndef RTC_RODATA_ATTR
#  define RTC_RODATA_ATTR
#endif

#endif /* RIVR_COMPAT_ESP_ATTR_H */
