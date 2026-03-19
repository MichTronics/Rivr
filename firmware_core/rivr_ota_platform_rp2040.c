/**
 * @file  firmware_core/rivr_ota_platform_rp2040.c
 * @brief RP2040 stub for the OTA platform persistence interface.
 *
 * The ESP32 implementation (rivr_ota_platform.c) uses ESP-IDF NVS.
 * The RP2040 has no NVS; this stub makes all OTA persistence operations
 * fail gracefully so the OTA core layer compiles and links without errors.
 *
 * Consequence: OTA program pushes will be accepted if signature is valid
 * but will NOT persist across resets.  A future revision may add LittleFS
 * or flash-based persistence.
 *
 * Only compiled when RIVR_PLATFORM_RP2040 is defined.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "rivr_ota_platform.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ── In-RAM state (lost on reset — no persistence on RP2040 yet) ─────────── */
static uint32_t s_ota_seq     = 0u;
static uint32_t s_ota_pending = 0u;

/* Program text buffer (volatile — reset clears it) */
#define OTA_PROG_BUF_MAX  2048u
static char     s_prog_buf[OTA_PROG_BUF_MAX];
static size_t   s_prog_len = 0u;

/* ── Anti-replay sequence number ─────────────────────────────────────────── */

uint32_t ota_platform_load_seq(void)
{
    return s_ota_seq;
}

bool ota_platform_save_seq(uint32_t seq)
{
    s_ota_seq = seq;
    return true;  /* saved in RAM; will be lost on reset */
}

/* ── Boot-confirm pending flag ───────────────────────────────────────────── */

uint32_t ota_platform_load_pending(void)
{
    return s_ota_pending;
}

bool ota_platform_save_pending(uint32_t v)
{
    s_ota_pending = v;
    return true;
}

/* ── Program-text storage ────────────────────────────────────────────────── */

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
               (unsigned)len, (unsigned)(OTA_PROG_BUF_MAX - 1u));
        return false;
    }
    memcpy(s_prog_buf, text, len);
    s_prog_buf[len] = '\0';
    s_prog_len = len;
    return true;
}

bool ota_storage_commit(void)
{
    /* On RP2040 we can't persist to flash yet; log a notice and return success
     * so the OTA core marks the push as accepted (in-RAM only). */
    printf("OTA: program stored in RAM (%u bytes) — not flash-persistent on RP2040\r\n",
           (unsigned)s_prog_len);
    return true;
}

#endif /* RIVR_PLATFORM_RP2040 */
