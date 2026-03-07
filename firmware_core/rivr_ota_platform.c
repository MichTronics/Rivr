/*
 * rivr_ota_platform.c — ESP-IDF NVS backend for the OTA platform interface.
 *
 * Implements the 7 functions declared in rivr_ota_platform.h using ESP-IDF
 * NVS for persistent storage.
 *
 * NEVER include this file in host / unit-test builds.  The tests directory
 * provides direct stub implementations of the same interface in test_ota.c.
 *
 * NVS layout (namespace "rivr")
 * ─────────────────────────────
 *   "ota_seq"     u32   last accepted OTA sequence number (anti-replay)
 *   "ota_pending" u32   boot-confirm flag (1 = awaiting confirm, 0 = clean)
 *
 * Program text is passed to rivr_nvs_store_program() which manages its own
 * NVS / flash key.  See rivr_layer/rivr_embed.c for that implementation.
 */

#include "rivr_ota_platform.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stddef.h>

#define NVS_NS      "rivr"
#define NVS_KEY_SEQ "ota_seq"
#define NVS_KEY_PND "ota_pending"

/* Forward declaration: implemented in rivr_layer/rivr_embed.c */
extern bool rivr_nvs_store_program(const char *src);

/* ── Anti-replay sequence number ─────────────────────────────────────────── */

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

/* ── Boot-confirm pending flag ───────────────────────────────────────────── */

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

/* ── Program-text storage (three-phase write) ────────────────────────────── */

/* Internal single-shot write buffer.  rivr_ota_activate() calls the three
 * storage functions in sequence: begin → write → commit.  The buffer is
 * sized to hold the largest expected RIVR program; payloads larger than
 * (sizeof - 1) bytes are silently truncated in ota_storage_write().        */
static char   s_prog_buf[256];
static size_t s_prog_len = 0u;

bool ota_storage_begin(void)
{
    s_prog_len     = 0u;
    s_prog_buf[0]  = '\0';
    return true;
}

bool ota_storage_write(const char *text, size_t len)
{
    if (!text) return false;
    /* Clamp to buffer capacity, leaving room for NUL terminator */
    size_t capacity = sizeof(s_prog_buf) - 1u;
    size_t n        = (len < capacity) ? len : capacity;
    memcpy(s_prog_buf, text, n);
    s_prog_buf[n] = '\0';
    s_prog_len    = n;
    return true;
}

bool ota_storage_commit(void)
{
    return rivr_nvs_store_program(s_prog_buf);
}
