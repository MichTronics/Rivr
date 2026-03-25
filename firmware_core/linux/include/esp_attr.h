/**
 * @file  esp_attr.h (Linux compat stub)
 * @brief Maps ESP-IDF placement attributes to nothing on Linux.
 */
#pragma once

/* IRAM_ATTR places code in IRAM on ESP32; unnecessary on Linux. */
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

/* RTC_DATA_ATTR places variables in RTC slow memory; no-op on Linux. */
#ifndef RTC_DATA_ATTR
#  define RTC_DATA_ATTR
#endif

/* DRAM_ATTR forces BSS/data section placement; no-op on Linux. */
#ifndef DRAM_ATTR
#  define DRAM_ATTR
#endif
