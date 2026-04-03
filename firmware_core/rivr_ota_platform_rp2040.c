/**
 * @file  firmware_core/rivr_ota_platform_rp2040.c
 * @brief RP2040 stub for the OTA platform persistence interface.
 *
 * Mirrors the nRF52 RAM-backed stub: OTA payloads are accepted for the current
 * session but are not persisted across reset yet.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "rivr_ota_platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_ota_seq = 0u;
static uint32_t s_ota_pending = 0u;

#define OTA_PROG_BUF_MAX 2048u
static char s_prog_buf[OTA_PROG_BUF_MAX];
static size_t s_prog_len = 0u;

uint32_t ota_platform_load_seq(void)
{
    return s_ota_seq;
}

bool ota_platform_save_seq(uint32_t seq)
{
    s_ota_seq = seq;
    return true;
}

uint32_t ota_platform_load_pending(void)
{
    return s_ota_pending;
}

bool ota_platform_save_pending(uint32_t v)
{
    s_ota_pending = v;
    return true;
}

bool ota_storage_begin(void)
{
    memset(s_prog_buf, 0, sizeof(s_prog_buf));
    s_prog_len = 0u;
    return true;
}

bool ota_storage_write(const char *text, size_t len)
{
    if (len >= OTA_PROG_BUF_MAX) {
        printf("OTA: program too large (%u bytes, max %u)\r\n",
               (unsigned)len,
               (unsigned)(OTA_PROG_BUF_MAX - 1u));
        return false;
    }
    memcpy(s_prog_buf, text, len);
    s_prog_buf[len] = '\0';
    s_prog_len = len;
    return true;
}

bool ota_storage_commit(void)
{
    printf("OTA: program stored in RAM (%u bytes) — not flash-persistent on RP2040\r\n",
           (unsigned)s_prog_len);
    return true;
}

#endif /* RIVR_PLATFORM_RP2040 */
