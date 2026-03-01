/**
 * @file  crypto_if.h
 * @brief Abstract cryptography HAL interface for RIVR firmware.
 *
 * PURPOSE
 * ───────
 * Provides a uniform crypto API so that protocol and OTA layers are not
 * coupled to a specific implementation (software ed25519, hardware AES,
 * or a future hardware security module).
 *
 * CURRENT IMPLEMENTATIONS
 * ───────────────────────
 * • Ed25519 signature verification — ed25519_verify.c (TweetNaCl-based,
 *   already present and guarded for Xtensa ABI).
 * • PSK-AES-128-CTR optional encryption — enabled with RIVR_FEATURE_ENCRYPTION=1
 *   (see feature_flags.h).  When disabled, encrypt/decrypt are no-ops that
 *   return the plaintext untouched.
 *
 * CRYPTO OBJECTS OVERVIEW
 * ───────────────────────
 *
 *  Ed25519 (asymmetric, signature only):
 *    crypto_if_ed25519_verify()  — verify a 64-byte signature
 *
 *  PSK-AES-128-CTR (symmetric, frame-level confidentiality):
 *    crypto_if_psk_encrypt()     — encrypt-in-place with nonce prepended
 *    crypto_if_psk_decrypt()     — detect & strip nonce, decrypt-in-place
 *
 * REPLAY PROTECTION
 * ─────────────────
 * The PSK layer does NOT implement replay protection — that is handled at
 * the routing layer via the dedupe cache (src_id + seq pair tracking).
 * The 4-byte nonce is a random value seeded from esp_random() (hardware RNG
 * on ESP32) to prevent ciphertext oracle attacks.
 *
 * MEMORY OVERHEAD
 * ───────────────
 * • RIVR_FEATURE_ENCRYPTION=0 (default): zero runtime cost, stubs inline.
 * • RIVR_FEATURE_ENCRYPTION=1: +400 bytes flash (AES core), +16 bytes RAM
 *   for key schedule, +4 bytes per frame for nonce overhead.
 *
 * ENABLING
 * ────────
 * Add to platformio.ini build_flags:
 *   -DRIVR_FEATURE_ENCRYPTION=1
 *   -DRIVR_PSK_HEX=\"0102030405060708090a0b0c0d0e0f10\"
 *
 * NOTE: Ed25519 OTA verification (rivr_ota.h) is independent of the PSK
 * frame encryption layer.  Both can be enabled simultaneously.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "feature_flags.h"
#include "../ed25519_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Ed25519 signature verification  (always available)
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Verify a 64-byte Ed25519 signature.
 *
 * Thin wrapper around rivr_ed25519_verify() from ed25519_verify.h.
 *
 * @param sig      64-byte signature.
 * @param msg      Message that was signed.
 * @param msg_len  Byte length of @p msg.
 * @param pubkey   32-byte Ed25519 public key.
 * @return true  → signature is valid.
 * @return false → invalid signature or bad parameters.
 */
static inline bool crypto_if_ed25519_verify(const uint8_t sig[64],
                                            const uint8_t *msg, size_t msg_len,
                                            const uint8_t pubkey[32])
{
    return rivr_ed25519_verify(sig, msg, (uint32_t)msg_len, pubkey);
}

/* ═══════════════════════════════════════════════════════════════════
 * PSK frame-level encryption  (RIVR_FEATURE_ENCRYPTION=1 required)
 * ═══════════════════════════════════════════════════════════════════ */

/** Size of the per-frame nonce prepended to encrypted payloads. */
#define CRYPTO_IF_NONCE_LEN   4u

/** AES-128 key size in bytes. */
#define CRYPTO_IF_KEY_LEN    16u

#if RIVR_FEATURE_ENCRYPTION

/**
 * @brief Initialise the PSK key schedule from the compile-time hex key.
 *
 * Must be called once before crypto_if_psk_encrypt/decrypt.
 * Key is read from RIVR_PSK_HEX (16 hex bytes, e.g. "0102...0f10").
 */
void crypto_if_psk_init(void);

/**
 * @brief Encrypt a frame payload in-place, prepending a 4-byte random nonce.
 *
 * The buffer must have at least CRYPTO_IF_NONCE_LEN extra bytes of headroom
 * BEFORE @p payload (i.e. the caller reserves space at payload[-4]).
 * After this call the nonce occupies bytes [-4..-1] relative to @p payload,
 * and @p payload is overwritten with ciphertext.
 *
 * @param payload      Pointer to start of plaintext (IN) / ciphertext (OUT).
 * @param payload_len  Length of payload in bytes.
 * @param nonce_out    4-byte nonce written here (same as prepended value).
 */
void crypto_if_psk_encrypt(uint8_t *payload, uint8_t payload_len,
                            uint8_t nonce_out[CRYPTO_IF_NONCE_LEN]);

/**
 * @brief Decrypt a frame payload in-place, consuming the leading nonce.
 *
 * @param nonce        4-byte nonce from the frame header area.
 * @param payload      Ciphertext buffer (overwritten with plaintext in-place).
 * @param payload_len  Length of payload in bytes.
 */
void crypto_if_psk_decrypt(const uint8_t nonce[CRYPTO_IF_NONCE_LEN],
                            uint8_t *payload, uint8_t payload_len);

#else /* RIVR_FEATURE_ENCRYPTION == 0 → stubs, zero runtime cost */

static inline void crypto_if_psk_init(void) {}

static inline void crypto_if_psk_encrypt(uint8_t *payload, uint8_t payload_len,
                                          uint8_t nonce_out[CRYPTO_IF_NONCE_LEN])
{
    (void)payload; (void)payload_len;
    /* No-op: write a deterministic nonce so callers don't see uninitialised bytes */
    nonce_out[0] = nonce_out[1] = nonce_out[2] = nonce_out[3] = 0xAAu;
}

static inline void crypto_if_psk_decrypt(const uint8_t nonce[CRYPTO_IF_NONCE_LEN],
                                          uint8_t *payload, uint8_t payload_len)
{
    (void)nonce; (void)payload; (void)payload_len; /* no-op */
}

#endif /* RIVR_FEATURE_ENCRYPTION */

#ifdef __cplusplus
}
#endif
