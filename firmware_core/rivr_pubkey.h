/*
 * rivr_pubkey.h — Static Ed25519 public-key ring for signed OTA program updates.
 *
 * Up to RIVR_OTA_KEY_COUNT keys are supported.  The key_id byte in the OTA
 * wire format selects which key to use for verification; key_id is inside the
 * signature coverage so it cannot be substituted post-hoc.
 *
 * Replace the keys below with real 32-byte compressed Ed25519 public keys
 * before building production firmware.  The matching private keys must never
 * be stored on device.
 *
 * Rotation strategy: when issuing an update signed with the new key, first
 * deploy a firmware that includes both the old (key 0) and new (key 1) keys.
 * Once all devices are on the new firmware, retire key 0 by replacing it
 * with the new key at index 0 and removing the old key 1 slot.
 *
 * Keys below were generated from the development seeds:
 *   key 0: deadbeef × 8  (matches existing test vectors)
 *   key 1: cafebabe × 8  (second dev key — never use in production)
 */
#pragma once
#include <stdint.h>

/** Number of public keys in the key ring (max 255 but practically 2). */
#define RIVR_OTA_KEY_COUNT  2u

/**
 * Ed25519 compressed public-key ring.
 * RIVR_OTA_KEYS[key_id] is the 32-byte public key for the given key_id.
 */
static const uint8_t RIVR_OTA_KEYS[RIVR_OTA_KEY_COUNT][32] = {
    { /* key 0 — dev key (deadbeef × 8) */
        0xff, 0x57, 0x57, 0x5d, 0xc7, 0xaf, 0x8b, 0xfc,
        0x4d, 0x08, 0x37, 0xcc, 0x1c, 0xe2, 0x01, 0x7b,
        0x68, 0x6a, 0x88, 0x14, 0x5d, 0xc5, 0x57, 0x9a,
        0x95, 0x8e, 0x34, 0x62, 0xfe, 0x9a, 0x90, 0x8e
    },
    { /* key 1 — dev key 2 (cafebabe × 8) — replace before production */
        0xaa, 0x73, 0x31, 0x87, 0xfe, 0xb4, 0xd4, 0x8a,
        0x0a, 0xf5, 0x65, 0x89, 0x0e, 0x96, 0x79, 0xca,
        0x43, 0x28, 0xe4, 0x59, 0x85, 0xdb, 0x9a, 0xb3,
        0x54, 0x58, 0xd3, 0xd1, 0x80, 0xb5, 0x24, 0x16
    },
};
