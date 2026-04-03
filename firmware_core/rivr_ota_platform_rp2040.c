/**
 * @file  firmware_core/rivr_ota_platform_rp2040.c
 * @brief RP2040 LittleFS-backed persistence interface for OTA metadata.
 *
 * Uses the RP2040 compat NVS API, which is implemented on top of LittleFS.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "rivr_ota_platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nvs.h"

#define NVS_NS      "rivr"
#define NVS_KEY_SEQ "ota_seq"
#define NVS_KEY_PND "ota_pending"

extern bool rivr_nvs_store_program(const char *src);

#define OTA_PROG_BUF_MAX 2048u
static char s_prog_buf[OTA_PROG_BUF_MAX];
static size_t s_prog_len = 0u;

uint32_t ota_platform_load_seq(void)
{
    nvs_handle_t h;
    uint32_t val = 0u;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_SEQ, &val);
        nvs_close(h);
    }
    return val;
}

bool ota_platform_save_seq(uint32_t seq)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_u32(h, NVS_KEY_SEQ, seq) == ESP_OK)
           && (nvs_commit(h)                    == ESP_OK);
    nvs_close(h);
    return ok;
}

uint32_t ota_platform_load_pending(void)
{
    nvs_handle_t h;
    uint32_t val = 0u;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_PND, &val);
        nvs_close(h);
    }
    return val;
}

bool ota_platform_save_pending(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_u32(h, NVS_KEY_PND, v) == ESP_OK)
           && (nvs_commit(h)                   == ESP_OK);
    nvs_close(h);
    return ok;
}

bool ota_storage_begin(void)
{
    memset(s_prog_buf, 0, sizeof(s_prog_buf));
    s_prog_len = 0u;
    return true;
}

bool ota_storage_write(const char *text, size_t len)
{
    if (!text) {
        return false;
    }
    if (len >= OTA_PROG_BUF_MAX) {
        return false;
    }
    memcpy(s_prog_buf, text, len);
    s_prog_buf[len] = '\0';
    s_prog_len = len;
    return true;
}

bool ota_storage_commit(void)
{
    return rivr_nvs_store_program(s_prog_buf);
}

#endif /* RIVR_PLATFORM_RP2040 */
