/**
 * @file  rivr_ota_platform.h
 * @brief Platform persistence interface for the OTA core layer.
 *
 * This header decouples rivr_ota_core.c from any ESP-IDF or NVS headers.
 * Two implementations exist:
 *
 *   rivr_ota_platform.c  — ESP-IDF NVS backend (firmware builds only)
 *   tests/test_ota.c     — Inline stubs for host-side unit tests
 *
 * INTERFACE GROUPS
 * ────────────────
 * Anti-replay sequence number:
 *   ota_platform_load_seq()  — read the last accepted OTA sequence number
 *   ota_platform_save_seq()  — persist a new sequence number after accept
 *
 * Boot-confirm pending flag:
 *   ota_platform_load_pending()  — read the ota_pending NVS flag
 *   ota_platform_save_pending()  — write the ota_pending NVS flag
 *
 * Program-text storage (three-phase write):
 *   ota_storage_begin()   — reset/prepare the write context
 *   ota_storage_write()   — buffer the extracted program text
 *   ota_storage_commit()  — persist the buffered text to NVS / flash
 *
 * ADDING A NEW PLATFORM
 * ─────────────────────
 * Provide a translation unit that implements all 7 functions declared here.
 * Link that TU instead of rivr_ota_platform.c.  No changes to the core or
 * the public rivr_ota.h API are required.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Anti-replay sequence number ────────────────────────────────────────── */

/**
 * Load the last accepted OTA sequence number from persistent storage.
 * Returns 0 when no value has been stored (first boot or storage error).
 */
uint32_t ota_platform_load_seq(void);

/**
 * Persist @p seq as the new last-accepted sequence number.
 * Returns true on success; false if the write fails (OTA will be rejected).
 */
bool ota_platform_save_seq(uint32_t seq);

/* ── Boot-confirm pending flag ───────────────────────────────────────────── */

/**
 * Load the ota_pending flag (0 = confirmed / no pending, 1 = pending confirm).
 */
uint32_t ota_platform_load_pending(void);

/**
 * Save @p v to the ota_pending flag.
 */
bool ota_platform_save_pending(uint32_t v);

/* ── Program-text storage (begin / write / commit) ──────────────────────── */

/**
 * Begin a new program-text write transaction.
 * Resets any partial state from a previous (possibly failed) write.
 * Must be called before ota_storage_write().
 */
bool ota_storage_begin(void);

/**
 * Buffer @p len bytes of program text from @p text.
 * May be called once per transaction (single-shot for current payload sizes).
 * @p text is NOT NUL-terminated; the implementation must add the NUL.
 */
bool ota_storage_write(const char *text, size_t len);

/**
 * Commit the buffered text to persistent storage (NVS, flash, …).
 * Returns true on success.  Must be called after ota_storage_write().
 */
bool ota_storage_commit(void);
