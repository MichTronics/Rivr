/**
 * @file  rivr_panic.h
 * @brief RIVR crash marker and reset-reason logging.
 *
 * PURPOSE
 * ───────
 * After a panic or watchdog reset the next boot cycle should emit a
 * structured log line so the operator knows what happened.  This module:
 *
 *   1. Writes a crash marker to RTC slow memory before a soft panic so it
 *      survives the warm reset.
 *   2. On every boot, reads the marker and emits a @CRASH JSON line if
 *      the previous session ended abnormally.
 *   3. Clears the marker after reporting.
 *
 * INTEGRATION
 * ───────────
 * Call rivr_panic_check_prev() once near the top of app_main(), BEFORE
 * any vTaskDelay, so the line appears at the start of the boot log.
 *
 * rivr_panic_mark() is a last-resort handler — call it from any context
 * where an unrecoverable firmware error is detected (e.g. SPI failure
 * streak, assertion failure) to annotate the reason before rebooting.
 *
 * EXAMPLE OUTPUT
 * ──────────────
 *   @CRASH {"reason":"WDT","uptime_ms":87432,"resets":3}
 *   @CRASH {"reason":"ASSERT:routing.c:142","uptime_ms":0,"resets":1}
 *
 * NOTE: The IDF task watchdog already reboots; this module reports AFTER
 * the reboot, not during it.  For WDT resets the reason field says "WDT"
 * and uptime_ms is 0 (RTC counter not populated in that path).
 *
 * MEMORY COST
 * ───────────
 * • 16 bytes in RTC slow memory (survives warm reset, cleared on cold boot).
 * • ~200 bytes of flash for the reporting function.
 * • Zero DRAM usage at runtime beyond the RTC region.
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of the reason string stored in the crash marker. */
#define RIVR_PANIC_REASON_MAX  12u

/**
 * @brief Write a crash marker to RTC memory for the next boot to find.
 *
 * Call this immediately before triggering a controlled reboot (e.g. after
 * detecting an unrecoverable hardware fault).  The IDF panic handler will
 * also trigger a reboot (via the task WDT), but in that case the reason
 * field is populated as "WDT" by rivr_panic_check_prev() using the IDF
 * reset-reason API.
 *
 * @param reason  Short ASCII reason string (max RIVR_PANIC_REASON_MAX-1 chars).
 *                Examples: "SPI_FAIL", "ASSERT", "STACK_OVF"
 */
void rivr_panic_mark(const char *reason);

/**
 * @brief Check for a crash marker from the previous boot and log it.
 *
 * Reads the IDF reset reason (esp_reset_reason()) and, if a RIVR crash
 * marker is present in RTC memory, emits a @CRASH JSON line via printf.
 * The marker is then cleared so it does not appear on the next normal reboot.
 *
 * Call ONCE from app_main() before starting any tasks.
 *
 * Output format:
 *   @CRASH {"reason":"<str>","uptime_ms":<n>,"resets":<n>,"idf_reason":<n>}
 *
 * where:
 *   reason      — rivr_panic_mark() reason string, or "WDT"/"POWERON"/etc.
 *   uptime_ms   — uptime at crash from RTC marker (0 if not set by firmware)
 *   resets      — consecutive abnormal reset counter (saturates at 255)
 *   idf_reason  — raw esp_reset_reason() code
 */
void rivr_panic_check_prev(void);

/**
 * @brief Increment the consecutive reset counter (called by rivr_panic_mark).
 * Saturates at 255.  Cleared to 0 on the first clean boot (uptime > 60 s).
 */
void rivr_panic_reset_count_inc(void);

/**
 * @brief Clear the consecutive reset counter after a clean run.
 * Call from the main loop after the node has been up for >60 seconds.
 */
void rivr_panic_clear_reset_count(void);

#ifdef __cplusplus
}
#endif
