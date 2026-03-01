/*
 * rivr_ota.h — Signed OTA RIVR program update gate.
 *
 * When RIVR_SIGNED_PROG is defined (set in CMakeLists / platformio.ini for
 * production builds) every PKT_PROG_PUSH payload must carry a valid Ed25519
 * signature and a monotonically-increasing sequence number.  Unsigned or
 * replayed pushes are silently rejected.
 *
 * Wire-format of the OTA payload (v1 + key-id extension):
 *
 *   [0..63]   Ed25519 signature over (key_id ‖ seq ‖ program_text)
 *   [64]      key_id    u8      index into RIVR_OTA_KEYS[] (0..RIVR_OTA_KEY_COUNT-1)
 *   [65..68]  uint32_t sequence number, little-endian
 *   [69..]    RIVR program text (UTF-8, NUL-terminated in NVS)
 *
 * Total overhead: 69 bytes.  PKT_PROG_PUSH payload_len covers the full
 * signed blob (sig + key_id + seq + text), so the raw uint8_t field limits
 * it to 255 bytes max (186 bytes of program text).  For larger programs use
 * the chunked push extension (not yet implemented).
 *
 * The key_id byte is inside the signature coverage so it cannot be
 * substituted without invalidating the signature.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RIVR_OTA_SIG_LEN   64u
#define RIVR_OTA_KEY_LEN    1u   /**< key_id field (1 byte, inside sig coverage) */
#define RIVR_OTA_SEQ_LEN    4u
#define RIVR_OTA_HDR_LEN   (RIVR_OTA_SIG_LEN + RIVR_OTA_KEY_LEN + RIVR_OTA_SEQ_LEN)  /* 69 */

/*
 * rivr_ota_verify() — Authenticate a signed OTA payload.
 *
 * @param payload     Raw PKT_PROG_PUSH payload (sig ‖ seq ‖ text).
 * @param payload_len Byte length of payload (must be > RIVR_OTA_HDR_LEN).
 *
 * Returns true when:
 *   1. payload_len > RIVR_OTA_HDR_LEN (minimum sanity check)
 *   2. The Ed25519 signature over (seq_bytes ‖ program_text) is valid for
 *      the key in rivr_pubkey.h.
 *   3. The 32-bit sequence number is strictly greater than the last accepted
 *      sequence stored in NVS (anti-replay).
 *
 * On success the new sequence number is persisted to NVS so it survives
 * reboot.  On failure nothing is written.
 *
 * When RIVR_SIGNED_PROG is not defined this function is a no-op that always
 * returns true (unsigned builds accept all pushes as before).
 */
bool rivr_ota_verify(const uint8_t *payload, size_t payload_len);

/*
 * rivr_ota_confirm() — Acknowledge successful boot from the new program.
 *
 * Call this once the newly pushed RIVR program has run without error.
 * Clears the "ota_pending" NVS flag so a subsequent reboot does not trigger
 * an automatic rollback to the previous program.
 *
 * Returns true when the NVS write succeeds (or in unsigned / host builds
 * where the flag is tracked in RAM).
 *
 * Rollback policy: if the device reboots while ota_pending == 1 (i.e. before
 * rivr_ota_confirm() is called), the platform layer should restore the
 * previous program from the "ota_prev" NVS key and clear ota_pending.
 * This is handled by firmware_core/main.c on ESP-IDF builds.
 */
bool rivr_ota_confirm(void);

/*
 * rivr_ota_is_pending() — Query whether an unconfirmed OTA update is active.
 *
 * Returns true when an OTA update has been activated but not yet confirmed
 * via rivr_ota_confirm().  Useful for displaying an "awaiting confirm"
 * status on the device display.
 */
bool rivr_ota_is_pending(void);

/*
 * rivr_ota_activate() — Extract program text from a verified payload and
 * store it via rivr_nvs_store_program().
 *
 * Call only after rivr_ota_verify() returns true.
 *
 * @param payload     Same pointer passed to rivr_ota_verify().
 * @param payload_len Same length passed to rivr_ota_verify().
 *
 * Returns true when rivr_nvs_store_program() succeeds.
 */
bool rivr_ota_activate(const uint8_t *payload, size_t payload_len);
