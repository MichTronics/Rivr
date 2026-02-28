/**
 * @file  ed25519_verify.h
 * @brief Minimal Ed25519 signature verification (verify-only, no keygen/sign).
 *
 * Pure C, no heap allocation, no floating-point, no external dependencies
 * beyond <stdint.h> and <string.h>.  Suitable for bare-metal ESP32 and
 * for host-side unit-test builds.
 *
 * Derived from TweetNaCl (Bernstein, Duif, Lange, Schwabe, Yang 2012).
 * TweetNaCl is public domain.  This file is public domain.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Verify a detached Ed25519 signature.
 *
 * @param sig    64-byte detached signature.
 * @param msg    Message that was signed.
 * @param msglen Length of @p msg in bytes.
 * @param pk     32-byte Ed25519 public key.
 * @return true  Signature is valid for the given message and public key.
 * @return false Signature is invalid (or key/signature are malformed).
 */
bool ed25519_verify_detached(const uint8_t sig[64],
                              const uint8_t *msg, size_t msglen,
                              const uint8_t pk[32]);

#ifdef __cplusplus
}
#endif
