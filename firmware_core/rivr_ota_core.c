/*
 * rivr_ota_core.c — Signed OTA RIVR program update gate (platform-agnostic core).
 *
 * See rivr_ota.h for the public API and wire-format description.
 *
 * ARCHITECTURE
 * ────────────
 * This translation unit is ESP-IDF-free.  All persistent-storage operations
 * are delegated to the platform interface declared in rivr_ota_platform.h:
 *
 *   ESP-IDF firmware  → link rivr_ota_platform.c  (NVS backend)
 *   Host unit tests   → link stub functions from the test file
 *
 * Do NOT add ESP-IDF / NVS / FreeRTOS includes here.
 *
 * DEPENDENCIES (all host-safe)
 * ─────────────────────────────
 *   rivr_ota.h           public API
 *   rivr_ota_platform.h  platform persistence interface
 *   rivr_pubkey.h        RIVR_OTA_KEYS[], RIVR_OTA_KEY_COUNT
 *   ed25519_verify.h     ed25519_verify_detached()
 *   hal/feature_flags.h  RIVR_SIGNED_PROG
 */

#include "rivr_ota.h"
#include "rivr_ota_platform.h"
#include "rivr_pubkey.h"
#include "ed25519_verify.h"
#include "hal/feature_flags.h"   /* RIVR_SIGNED_PROG */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Public API ─────────────────────────────────────────────────────────── */

bool rivr_ota_verify(const uint8_t *payload, size_t payload_len)
{
#if !RIVR_SIGNED_PROG
    /* Unsigned build: minimum sanity only — accept everything with data */
    if (!payload || payload_len <= RIVR_OTA_HDR_LEN) return false;
    (void)payload;
    return true;
#else
    if (!payload || payload_len <= RIVR_OTA_HDR_LEN) return false;

    /* ── 1. Parse payload: sig | key_id | seq_bytes | program_text ── */
    const uint8_t *sig       = payload;
    uint8_t        key_id    = payload[RIVR_OTA_SIG_LEN];
    const uint8_t *seq_bytes = payload + RIVR_OTA_SIG_LEN + RIVR_OTA_KEY_LEN;

    /* Reject out-of-range key_id before any crypto work */
    if (key_id >= RIVR_OTA_KEY_COUNT) return false;
    const uint8_t *pub_key = RIVR_OTA_KEYS[key_id];

    /*
     * The signed message is (key_id ‖ seq_bytes ‖ program_text):
     * everything after the 64-byte signature.  key_id is inside the
     * signature coverage so an attacker cannot substitute it post-hoc.
     *
     * Guard explicitly before the subtraction: even though the earlier
     * payload_len > RIVR_OTA_HDR_LEN check already ensures
     * payload_len > 69 > 64 = RIVR_OTA_SIG_LEN, an explicit local guard
     * keeps the invariant visible and prevents a size_t underflow if
     * this code is ever refactored or the header length changes.
     */
    if (payload_len < RIVR_OTA_SIG_LEN) return false;   /* underflow guard */
    uint8_t signed_msg[256];
    size_t  signed_len = payload_len - RIVR_OTA_SIG_LEN;
    if (signed_len > sizeof(signed_msg)) return false;
    __builtin_memcpy(signed_msg, payload + RIVR_OTA_SIG_LEN, signed_len);

    if (!ed25519_verify_detached(sig, signed_msg, signed_len, pub_key)) {
        return false;   /* signature mismatch */
    }

    /* ── 2. Anti-replay: seq must be strictly greater than last accepted ── */
    uint32_t seq = (uint32_t)seq_bytes[0]
                 | ((uint32_t)seq_bytes[1] <<  8)
                 | ((uint32_t)seq_bytes[2] << 16)
                 | ((uint32_t)seq_bytes[3] << 24);

    uint32_t last = ota_platform_load_seq();
    if (seq <= last) return false;

    /* ── 3. Persist new seq so a reboot does not allow replay ── */
    if (!ota_platform_save_seq(seq)) return false;

    return true;
#endif /* !RIVR_SIGNED_PROG */
}

bool rivr_ota_activate(const uint8_t *payload, size_t payload_len)
{
    if (!payload || payload_len <= RIVR_OTA_HDR_LEN) return false;

    /* Program text starts immediately after the fixed header */
    const char *text     = (const char *)(payload + RIVR_OTA_HDR_LEN);
    size_t      text_len = payload_len - RIVR_OTA_HDR_LEN;

    /* Three-phase write through the platform storage interface */
    if (!ota_storage_begin())               return false;
    if (!ota_storage_write(text, text_len)) return false;
    if (!ota_storage_commit())              return false;

    /*
     * Mark the update as pending-confirm.  If the node reboots before
     * rivr_ota_confirm() is called the platform layer can restore the
     * previous program from the "ota_prev" NVS key (handled by main.c).
     */
    (void)ota_platform_save_pending(1u);
    return true;
}

bool rivr_ota_confirm(void)
{
    return ota_platform_save_pending(0u);
}

bool rivr_ota_is_pending(void)
{
    return ota_platform_load_pending() != 0u;
}
