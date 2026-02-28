/**
 * @file  test_ota.c
 * @brief Unit tests for the P2 signed OTA gate (rivr_ota.c).
 *
 * Tests:
 *   1. Valid payload (seq=1)          → rivr_ota_verify() returns true
 *   2. Anti-replay (seq=1 again)      → second verify returns false
 *   3. Valid seq=2                    → verify returns true (seq advances)
 *   4. Truncated payload              → verify returns false
 *   5. Tampered signature             → verify returns false
 *   6. Tampered seq bytes             → verify returns false
 *   7. rivr_ota_activate()            → stub receives NUL-terminated text
 *   8. Unsigned build (#undef)        → rivr_ota_verify() always true
 *
 * Test vectors were generated with:
 *   seed = bytes([0xde,0xad,0xbe,0xef]*8)   (matches rivr_pubkey.h dev key)
 *   from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
 *   key = Ed25519PrivateKey.from_private_bytes(seed)
 *   payload = key.sign(seq_le + text) + seq_le + text
 *
 * Build (from project root, signed path):
 *   gcc -O2 -Ifirmware_core -DIRAM_ATTR="" -DRIVR_SIGNED_PROG \
 *       firmware_core/ed25519_verify.c \
 *       firmware_core/rivr_ota.c \
 *       tests/test_ota.c \
 *       -o /tmp/test_ota && /tmp/test_ota
 *
 * Build (unsigned path — test 8 only):
 *   gcc -O2 -Ifirmware_core -DIRAM_ATTR="" \
 *       firmware_core/ed25519_verify.c \
 *       firmware_core/rivr_ota.c \
 *       tests/test_ota.c \
 *       -o /tmp/test_ota_unsigned && /tmp/test_ota_unsigned
 */

#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "rivr_ota.h"    /* RIVR_OTA_HDR_LEN etc. — firmware_core/ on path */

/* ── Stub implementations required by rivr_ota.c ───────────────────────── */

/* NVS anti-replay stubs */
static uint32_t s_stub_seq = 0u;

uint32_t ota_stub_load_seq(void)       { return s_stub_seq; }
bool     ota_stub_save_seq(uint32_t s) { s_stub_seq = s; return true; }

/* NVS program storage stub */
static char  s_nvs_program[256];
static bool  s_nvs_store_called = false;

bool rivr_nvs_store_program(const char *src)
{
    s_nvs_store_called = true;
    strncpy(s_nvs_program, src, sizeof(s_nvs_program) - 1);
    s_nvs_program[sizeof(s_nvs_program) - 1] = '\0';
    return true;
}

/* ── Minimal assertion framework ────────────────────────────────────────── */
static uint32_t s_pass = 0u;
static uint32_t s_fail = 0u;

#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }          \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg),              \
                        __FILE__, __LINE__); s_fail++; }            \
} while (0)

/* ── Test vectors ────────────────────────────────────────────────────────── *
 * VEC1: sig over (seq=1 LE | "x=1") signed with deadbeef×8 key             *
 * VEC2: sig over (seq=2 LE | "y=2") signed with deadbeef×8 key             *
 * ─────────────────────────────────────────────────────────────────────────*/

/* VEC1: payload = sign(seq1|"x=1") | seq1 | "x=1"  (71 bytes total) */
static const uint8_t VEC1[] = {
    /* Ed25519 signature (64 bytes) */
    0x95, 0xff, 0x9f, 0x87, 0xeb, 0x7b, 0x54, 0xab,
    0xc9, 0xb3, 0x60, 0x45, 0x76, 0x7e, 0x97, 0x60,
    0x3a, 0xb6, 0x51, 0xf1, 0xfb, 0x74, 0x82, 0x81,
    0x31, 0x28, 0xba, 0x8d, 0x81, 0xbe, 0x57, 0x98,
    0x04, 0x11, 0x37, 0xd4, 0xf0, 0xf7, 0xcf, 0x0c,
    0xbe, 0x70, 0xba, 0x06, 0x4c, 0x50, 0x9a, 0x0d,
    0xfe, 0xb5, 0xf1, 0x9b, 0x32, 0xfb, 0x59, 0x53,
    0xb9, 0x4d, 0xf5, 0x8e, 0xc6, 0xf8, 0x5c, 0x00,
    /* seq = 1 (LE uint32) */
    0x01, 0x00, 0x00, 0x00,
    /* program text: "x=1" */
    0x78, 0x3d, 0x31
};

/* VEC2: payload = sign(seq2|"y=2") | seq2 | "y=2"  (71 bytes total) */
static const uint8_t VEC2[] = {
    /* Ed25519 signature (64 bytes) */
    0x8f, 0x01, 0xb5, 0xdc, 0x1a, 0x6f, 0xde, 0xe1,
    0xed, 0x01, 0x46, 0x49, 0x28, 0x5f, 0xed, 0xea,
    0x08, 0x22, 0xa8, 0xb2, 0xbf, 0x26, 0xdc, 0xbf,
    0x8b, 0xf2, 0xc9, 0x9b, 0x64, 0x87, 0x83, 0xff,
    0x19, 0x2c, 0x29, 0x93, 0xcb, 0xf7, 0x99, 0x83,
    0x31, 0xf5, 0xa3, 0x65, 0xf3, 0x61, 0xc4, 0x90,
    0x51, 0x70, 0x9e, 0xaf, 0x9f, 0x36, 0xd0, 0xcb,
    0x73, 0x26, 0x52, 0x7e, 0xf9, 0x28, 0x12, 0x05,
    /* seq = 2 (LE uint32) */
    0x02, 0x00, 0x00, 0x00,
    /* program text: "y=2" */
    0x79, 0x3d, 0x32
};

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 1 — Valid first payload (seq=1) is accepted
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_valid_first(void)
{
    printf("\n=== TEST 1: Valid first payload (seq=1) ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 0u;
    bool ok = rivr_ota_verify(VEC1, sizeof(VEC1));
    CHECK(ok,              "verify VEC1 (seq=1) returns true");
    CHECK(s_stub_seq == 1u, "seq persisted as 1 after accept");
#else
    CHECK(rivr_ota_verify(VEC1, sizeof(VEC1)), "unsigned build: always true");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 2 — Anti-replay: same seq rejected
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_replay_rejected(void)
{
    printf("\n=== TEST 2: Replay rejected ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 1u;   /* pretend seq=1 already accepted */
    bool ok = rivr_ota_verify(VEC1, sizeof(VEC1));
    CHECK(!ok, "replay of seq=1 rejected when last_seq==1");

    /* seq=0 also rejected */
    s_stub_seq = 5u;
    uint8_t bad[sizeof(VEC1)];
    memcpy(bad, VEC1, sizeof(VEC1));
    /* Overwrite seq bytes to 0 */
    bad[64] = 0; bad[65] = 0; bad[66] = 0; bad[67] = 0;
    CHECK(!rivr_ota_verify(bad, sizeof(bad)), "old seq=0 rejected when last_seq==5");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 3 — Valid seq=2 accepted after seq=1
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_seq_advances(void)
{
    printf("\n=== TEST 3: Seq advances (seq=2 after seq=1) ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 1u;
    bool ok = rivr_ota_verify(VEC2, sizeof(VEC2));
    CHECK(ok,              "verify VEC2 (seq=2) after last=1 → true");
    CHECK(s_stub_seq == 2u, "seq persisted as 2 after accept");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 4 — Truncated payload rejected
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_truncated(void)
{
    printf("\n=== TEST 4: Truncated payload ===\n");
#ifdef RIVR_SIGNED_PROG
    /* At exactly HDR_LEN bytes there is no text — must be rejected */
    CHECK(!rivr_ota_verify(VEC1, RIVR_OTA_HDR_LEN),
          "payload == HDR_LEN (no text) → rejected");
    /* Below HDR_LEN */
    CHECK(!rivr_ota_verify(VEC1, RIVR_OTA_HDR_LEN - 1),
          "payload < HDR_LEN → rejected");
    /* Empty payload */
    CHECK(!rivr_ota_verify(VEC1, 0u), "empty payload → rejected");
    CHECK(!rivr_ota_verify(NULL, sizeof(VEC1)), "NULL pointer → rejected");
#else
    CHECK(!rivr_ota_verify(VEC1, RIVR_OTA_HDR_LEN), "truncated unsigned → rejected");
    CHECK(!rivr_ota_verify(NULL, sizeof(VEC1)),      "NULL unsigned → rejected");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 5 — Tampered signature rejected
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_tampered_sig(void)
{
    printf("\n=== TEST 5: Tampered signature ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 0u;
    uint8_t bad[sizeof(VEC1)];
    memcpy(bad, VEC1, sizeof(VEC1));
    bad[0] ^= 0xFFu;  /* flip first byte of sig */
    CHECK(!rivr_ota_verify(bad, sizeof(bad)), "tampered sig byte → rejected");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 6 — Tampered seq bytes rejected (sig mismatch)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_tampered_seq(void)
{
    printf("\n=== TEST 6: Tampered seq bytes ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 0u;
    uint8_t bad[sizeof(VEC1)];
    memcpy(bad, VEC1, sizeof(VEC1));
    bad[64] = 0x99u;  /* corrupt first seq byte */
    CHECK(!rivr_ota_verify(bad, sizeof(bad)),
          "tampered seq byte → sig mismatch → rejected");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 7 — rivr_ota_activate() stores NUL-terminated text
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_activate(void)
{
    printf("\n=== TEST 7: rivr_ota_activate() ===\n");
    s_nvs_store_called = false;
    memset(s_nvs_program, 0, sizeof(s_nvs_program));

    bool ok = rivr_ota_activate(VEC1, sizeof(VEC1));
    CHECK(ok,                           "activate returns true");
    CHECK(s_nvs_store_called,           "nvs_store_program was called");
    CHECK(strcmp(s_nvs_program, "x=1") == 0,
                                        "stored program text == \"x=1\"");

    /* Truncated payload → activate should fail */
    ok = rivr_ota_activate(VEC1, RIVR_OTA_HDR_LEN);
    CHECK(!ok, "activate with payload == HDR_LEN → false");
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
#ifdef RIVR_SIGNED_PROG
    printf("=== RIVR OTA unit tests (SIGNED build) ===\n");
#else
    printf("=== RIVR OTA unit tests (UNSIGNED build) ===\n");
#endif

    test_valid_first();
    test_replay_rejected();
    test_seq_advances();
    test_truncated();
    test_tampered_sig();
    test_tampered_seq();
    test_activate();

    printf("\n%u PASS  %u FAIL\n", (unsigned)s_pass, (unsigned)s_fail);
    return (s_fail == 0u) ? 0 : 1;
}
