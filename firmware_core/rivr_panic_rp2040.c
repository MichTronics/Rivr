/**
 * @file  firmware_core/rivr_panic_rp2040.c
 * @brief Stub crash marker for RP2040.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "rivr_panic.h"
#include "timebase.h"

#include <hardware/watchdog.h>
#include <stdio.h>
#include <string.h>

static char s_panic_reason[RIVR_PANIC_REASON_MAX] = {0};
static uint32_t s_panic_uptime_ms = 0u;
static uint8_t s_panic_valid = 0u;

void rivr_panic_check_prev(void)
{
    (void)s_panic_reason;
    (void)s_panic_uptime_ms;
    (void)s_panic_valid;
}

void rivr_panic_mark(const char *reason)
{
    strncpy(s_panic_reason, reason ? reason : "unknown", RIVR_PANIC_REASON_MAX - 1u);
    s_panic_reason[RIVR_PANIC_REASON_MAX - 1u] = '\0';
    s_panic_uptime_ms = tb_millis();
    s_panic_valid = 1u;

    printf("@CRASH {\"reason\":\"%s\",\"uptime_ms\":%u}\r\n",
           s_panic_reason,
           (unsigned)s_panic_uptime_ms);
    fflush(stdout);

    watchdog_reboot(0, 0, 0);
    while (1) {}
}

void rivr_panic_reset_count_inc(void)
{
}

void rivr_panic_clear_reset_count(void)
{
}

#endif /* RIVR_PLATFORM_RP2040 */
