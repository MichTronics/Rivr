/**
 * @file  crypto/hmac_sha256.h
 * @brief Self-contained SHA-256 and HMAC-SHA-256 for RIVR firmware.
 *
 * PURPOSE
 * ───────
 * Provides a minimal, dependency-free HMAC-SHA-256 implementation for use
 * in parameter-update signature verification (RIVR_FEATURE_SIGNED_PARAMS).
 * Follows the same pattern as ed25519_verify.c: public-domain algorithm,
 * no heap, no FreeRTOS, callable from main-loop context only.
 *
 * ALGORITHM
 * ─────────
 * SHA-256: FIPS 180-4 compliant, adapted from Brad Conte's public-domain
 * reference (https://github.com/B-Con/crypto-algorithms).
 * HMAC:    RFC 2104 — HMAC(K, m) = H((K⊕opad) ∥ H((K⊕ipad) ∥ m)).
 *
 * CONSTRAINTS
 * ───────────
 * • No heap allocation — all working state is on the caller's stack.
 * • No global mutable state — all context lives in rivr_sha256_ctx_t.
 * • Safe to call from any main-loop code; NOT ISR-safe.
 * • Maximum message length: 2^64 − 1 bits. For @PARAMS (≤ 255 bytes) there
 *   is no practical limit.
 *
 * STACK COST
 * ──────────
 * rivr_sha256():      ~196 bytes (sha256_ctx_t on stack)
 * rivr_hmac_sha256(): ~460 bytes (2 × key pads + 2 × ctx + inner hash)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 digest size in bytes. */
#define RIVR_SHA256_DIGEST_LEN  32u

/** HMAC-SHA-256 tag size in bytes (equals digest size). */
#define RIVR_HMAC_SHA256_LEN    32u

/**
 * @brief Compute SHA-256 over a contiguous byte buffer.
 *
 * @param data     Input data.
 * @param len      Byte length of @p data.
 * @param out      32-byte output buffer for the digest.
 */
void rivr_sha256(const uint8_t *data, size_t len, uint8_t out[RIVR_SHA256_DIGEST_LEN]);

/**
 * @brief Compute HMAC-SHA-256.
 *
 * @param key      Raw key bytes (any length; hashed internally if > 64 bytes).
 * @param key_len  Byte length of @p key.
 * @param msg      Message buffer.
 * @param msg_len  Byte length of @p msg.
 * @param out      32-byte output buffer for the MAC.
 */
void rivr_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *msg, size_t msg_len,
                      uint8_t out[RIVR_HMAC_SHA256_LEN]);

#ifdef __cplusplus
}
#endif
