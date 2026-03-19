/**
 * @file  firmware_core/rivr_panic_rp2040.c
 * @brief Stub crash marker for RP2040.
 *
 * The ESP32 rivr_panic.c uses RTC slow memory + esp_system.h APIs that are
 * not available on RP2040.  This stub provides the same interface with a
 * Serial-based crash report and hardware reboot.
 *
 * Only compiled when RIVR_PLATFORM_RP2040 is defined.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "rivr_panic.h"
#include "timebase.h"
#include <stdio.h>
#include <string.h>
#include <hardware/watchdog.h>

/* RP2040 has no RTC slow memory; BSS resets on cold boot. */
static char     s_panic_reason[RIVR_PANIC_REASON_MAX] = {0};
static uint32_t s_panic_uptime_ms = 0;
static uint8_t  s_panic_valid = 0;

void rivr_panic_check_prev(void)
{
    /* No warm-reset persistence on RP2040 without additional storage.
     * Always reports no previous crash. */
    (void)s_panic_reason;
    (void)s_panic_uptime_ms;
    (void)s_panic_valid;
}

void rivr_panic_mark(const char *reason)
{
    strncpy(s_panic_reason, reason ? reason : "unknown", RIVR_PANIC_REASON_MAX - 1);
    s_panic_reason[RIVR_PANIC_REASON_MAX - 1] = '\0';
    s_panic_uptime_ms = tb_millis();
    s_panic_valid = 1;

    printf("@CRASH {\"reason\":\"%s\",\"uptime_ms\":%u}\r\n",
           s_panic_reason, (unsigned)s_panic_uptime_ms);

    /* Trigger a hardware reboot via the RP2040 watchdog */
    watchdog_reboot(0, 0, 1);  /* normal reboot after 1 ms */
    for (;;) {} /* should not reach here */
}

#endif /* RIVR_PLATFORM_RP2040 */
