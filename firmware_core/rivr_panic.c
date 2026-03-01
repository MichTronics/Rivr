/**
 * @file  rivr_panic.c
 * @brief RIVR crash marker implementation (RTC slow memory + IDF reset API).
 */

#include "rivr_panic.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_system.h"

/* ── RTC crash marker layout ─────────────────────────────────────────────
 *
 * Stored in RTC_SLOW_ATTR memory — survives a warm (software/WDT) reset
 * but is zeroed by a cold power-on reset.
 *
 * Magic sentinel: 0xC4A5 — if this value is present the marker is valid.
 * ─────────────────────────────────────────────────────────────────────── */

#define RIVR_PANIC_MAGIC  0xC4A5u

typedef struct {
    uint16_t magic;                        /**< RIVR_PANIC_MAGIC if valid    */
    uint8_t  resets;                       /**< Consecutive abnormal resets  */
    uint8_t  _pad;
    uint32_t uptime_ms;                    /**< tb_millis() at crash time    */
    char     reason[RIVR_PANIC_REASON_MAX];/**< NUL-terminated reason string */
} rivr_crash_marker_t;

/* Placed in RTC slow memory — survives warm reset. */
static RTC_DATA_ATTR rivr_crash_marker_t s_crash;

/* ── IDF reset-reason to human string ──────────────────────────────────── */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_RST";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void rivr_panic_mark(const char *reason)
{
    s_crash.magic     = RIVR_PANIC_MAGIC;
    s_crash.uptime_ms = 0; /* caller may fill via tb_millis() before calling */
    if (s_crash.resets < 255u) s_crash.resets++;
    strncpy(s_crash.reason, reason ? reason : "UNKNOWN",
            RIVR_PANIC_REASON_MAX - 1u);
    s_crash.reason[RIVR_PANIC_REASON_MAX - 1u] = '\0';
}

void rivr_panic_reset_count_inc(void)
{
    if (s_crash.resets < 255u) s_crash.resets++;
}

void rivr_panic_clear_reset_count(void)
{
    s_crash.resets = 0;
    /* Leave magic intact so a subsequent WDT still gets reported. */
}

void rivr_panic_check_prev(void)
{
    esp_reset_reason_t idf_reason = esp_reset_reason();
    bool abnormal = (idf_reason == ESP_RST_PANIC)   ||
                    (idf_reason == ESP_RST_INT_WDT)  ||
                    (idf_reason == ESP_RST_TASK_WDT) ||
                    (idf_reason == ESP_RST_WDT)      ||
                    (idf_reason == ESP_RST_BROWNOUT);

    bool marker_valid = (s_crash.magic == RIVR_PANIC_MAGIC);

    /* Determine reported reason string */
    const char *reason;
    if (marker_valid && s_crash.reason[0] != '\0') {
        reason = s_crash.reason;
    } else if (abnormal) {
        reason = reset_reason_str(idf_reason);
    } else {
        /* Normal boot — nothing to report */
        if (!abnormal) {
            /* Clear stale marker from a previous run so it doesn't linger */
            if (marker_valid) {
                s_crash.magic = 0;
                memset(s_crash.reason, 0, sizeof(s_crash.reason));
            }
            return;
        }
        reason = "UNKNOWN";
    }

    uint32_t uptime = marker_valid ? s_crash.uptime_ms : 0u;
    uint8_t  resets = marker_valid ? s_crash.resets     : 1u;

    printf("@CRASH {\"reason\":\"%s\",\"uptime_ms\":%"PRIu32
           ",\"resets\":%u,\"idf_reason\":%d}\r\n",
           reason, uptime, (unsigned)resets, (int)idf_reason);

    /* Clear the marker after reporting */
    s_crash.magic     = 0;
    s_crash.uptime_ms = 0;
    memset(s_crash.reason, 0, sizeof(s_crash.reason));
    /* Keep resets counter — rivr_panic_clear_reset_count() clears it on
     * a clean long-running boot. */
}
