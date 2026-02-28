/*
 * rivr_ota.h — Signed OTA RIVR program update gate.
 *
 * When RIVR_SIGNED_PROG is defined (set in CMakeLists / platformio.ini for
 * production builds) every PKT_PROG_PUSH payload must carry a valid Ed25519
 * signature and a monotonically-increasing sequence number.  Unsigned or
 * replayed pushes are silently rejected.
 *
 * Wire-format of the OTA payload:
 *
 *   [0..63]   Ed25519 signature over (seq ‖ program_text)
 *   [64..67]  uint32_t sequence number, little-endian
 *   [68..]    RIVR program text (UTF-8, NUL-terminated in NVS)
 *
 * Total overhead: 68 bytes.  PKT_PROG_PUSH payload_len covers the full
 * signed blob (sig + seq + text), so the raw uint8_t field limits it to
 * 255 bytes max (187 bytes of program text).  For larger programs use the
 * chunked push extension (not yet implemented).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RIVR_OTA_SIG_LEN   64u
#define RIVR_OTA_SEQ_LEN    4u
#define RIVR_OTA_HDR_LEN   (RIVR_OTA_SIG_LEN + RIVR_OTA_SEQ_LEN)  /* 68 */

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
