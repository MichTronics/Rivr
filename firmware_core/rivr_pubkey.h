/*
 * rivr_pubkey.h — Static Ed25519 public key for signed OTA program updates.
 *
 * Replace RIVR_OTA_PUBLIC_KEY with the real 32-byte compressed Ed25519 public
 * key before building production firmware.  The matching private key must
 * never be stored on device.
 *
 * The public key below was generated from the development seed
 * (deadbeef × 8) and is safe to use only in lab / test builds.
 */
#pragma once
#include <stdint.h>

/* Ed25519 compressed public key (32 bytes, little-endian y-coordinate with
 * sign bit of x in the MSB of byte 31).                                    */
static const uint8_t RIVR_OTA_PUBLIC_KEY[32] = {
    0xff, 0x57, 0x57, 0x5d, 0xc7, 0xaf, 0x8b, 0xfc,
    0x4d, 0x08, 0x37, 0xcc, 0x1c, 0xe2, 0x01, 0x7b,
    0x68, 0x6a, 0x88, 0x14, 0x5d, 0xc5, 0x57, 0x9a,
    0x95, 0x8e, 0x34, 0x62, 0xfe, 0x9a, 0x90, 0x8e
};
