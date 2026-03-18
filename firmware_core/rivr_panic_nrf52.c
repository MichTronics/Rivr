/**
 * @file  firmware_core/rivr_panic_nrf52.c
 * @brief Stub crash marker for nRF52840.
 *
 * The ESP32 rivr_panic.c uses RTC slow memory + esp_system.h APIs that are
 * not available on nRF52.  This stub provides the same interface so the
 * linker is satisfied, with a simple Serial-based crash report instead.
 */

#if defined(RIVR_PLATFORM_NRF52840)

#include "rivr_panic.h"
#include "timebase.h"
#include <stdio.h>
#include <string.h>

/* nRF52 has no RTC slow memory; we use a BSS variable that resets on cold boot. */
static char s_panic_reason[RIVR_PANIC_REASON_MAX] = {0};
static uint32_t s_panic_uptime_ms = 0;
static uint8_t  s_panic_valid = 0;

void rivr_panic_check_prev(void)
{
    /* On nRF52 there is no warm-reset persistence without additional storage.
     * This stub always reports no previous crash. */
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

    /* Perform a hard reset */
    NVIC_SystemReset();
}

#endif /* RIVR_PLATFORM_NRF52840 */
