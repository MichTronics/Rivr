/**
 * @file  rivr_panic_linux.c
 * @brief Panic handler for Linux — logs the reason and calls abort().
 *
 * Replaces rivr_panic.c (ESP32) and rivr_panic_nrf52.c on Linux builds.
 * No RTC slow memory or hardware reset — previous crash info is not
 * persisted across process restarts.
 */

#if defined(RIVR_PLATFORM_LINUX)

#include "rivr_panic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rivr_panic_check_prev(void)
{
    /* No persistent crash marker on Linux — nothing to report. */
}

void rivr_panic_mark(const char *reason)
{
    fprintf(stderr, "\n[RIVR PANIC] %s\n", reason ? reason : "unknown");
    fflush(stderr);
    abort();
}

void rivr_panic_reset_count_inc(void)
{
    /* No persistent counter on Linux. */
}

void rivr_panic_clear_reset_count(void)
{
    /* No persistent counter on Linux. */
}

#endif /* RIVR_PLATFORM_LINUX */
