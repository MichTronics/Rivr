/**
 * @file  esp_system.h  (nRF52840 compat shim)
 * @brief Stub for ESP-IDF esp_system.h used on the nRF52840 Arduino build.
 *
 * Only the symbols actually referenced in the shared codebase are provided.
 */

#ifndef RIVR_COMPAT_ESP_SYSTEM_H
#define RIVR_COMPAT_ESP_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * esp_restart() – trigger a system reset.
 *
 * On Cortex-M4 we write SYSRESETREQ into SCB->AIRCR.
 * This is the same register write that CMSIS NVIC_SystemReset() performs,
 * expressed here without requiring the full CMSIS header.
 */
static inline __attribute__((noreturn)) void esp_restart(void)
{
    /* Cortex-M Application Interrupt and Reset Control Register */
    *((volatile unsigned int *)0xE000ED0CUL) =
        (unsigned int)((0x5FAUL << 16u) | (1u << 2u));  /* VECTKEY | SYSRESETREQ */
    for (;;) {} /* should never reach here */
}

/** Placeholder — not used on nRF52. */
static inline const char *esp_get_idf_version(void) { return "nRF52-Arduino"; }

/** Reset reason codes — provide minimal subset used by rivr_panic.h. */
typedef enum {
    ESP_RST_UNKNOWN   = 0,
    ESP_RST_POWERON   = 1,
    ESP_RST_SW        = 3,
    ESP_RST_PANIC     = 4,
    ESP_RST_WDT       = 6,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_UNKNOWN; }

#ifdef __cplusplus
}
#endif

#endif /* RIVR_COMPAT_ESP_SYSTEM_H */
