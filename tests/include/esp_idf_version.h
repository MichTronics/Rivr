/* esp_idf_version.h — host-build stub (tests/ only) */
#pragma once

#define ESP_IDF_VERSION_VAL(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))

/* Report IDF 5.1 so version-gated code takes the v5 path. */
#define ESP_IDF_VERSION   ESP_IDF_VERSION_VAL(5, 1, 0)
