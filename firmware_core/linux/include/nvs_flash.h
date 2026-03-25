/**
 * @file  nvs_flash.h (Linux compat stub)
 * @brief NVS flash init stub — always succeeds, does nothing.
 */
#pragma once
#include "esp_err.h"

static inline esp_err_t nvs_flash_init(void)  { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }
