/**
 * @file  esp_err.h (Linux compat stub)
 * @brief Minimal ESP-IDF error type shim for the Raspberry Pi Linux port.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef int32_t esp_err_t;

#define ESP_OK                           0
#define ESP_FAIL                        -1
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_NVS_NO_FREE_PAGES  0x1100
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1101

#define ESP_ERROR_CHECK(x) \
    do { \
        esp_err_t _e = (x); \
        if (_e != ESP_OK) { \
            fprintf(stderr, "[ESP_ERROR_CHECK] err=%d at %s:%d\n", \
                    (int)_e, __FILE__, __LINE__); \
        } \
    } while (0)
