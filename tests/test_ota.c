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

/* NVS ota_pending stubs (P3.3 boot-confirm) */
static uint32_t s_stub_pending = 0u;

uint32_t ota_stub_load_pending(void)         { return s_stub_pending; }
bool     ota_stub_save_pending(uint32_t v)   { s_stub_pending = v; return true; }

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

/* VEC1: payload = sign(key_id=0|seq1|"x=1") | key_id=0 | seq1 | "x=1"  (72 bytes) */
static const uint8_t VEC1[] = {
    /* Ed25519 signature (64 bytes) over (key_id=0 | seq=1 LE | "x=1") */
    0x6c, 0x82, 0xc6, 0xff, 0xe9, 0x00, 0x3f, 0xeb,
    0x93, 0xd5, 0x47, 0x76, 0xe4, 0xb8, 0x5e, 0xf2,
    0x04, 0x5e, 0x5f, 0x5d, 0xf6, 0x1d, 0x93, 0x7c,
    0xbf, 0x4f, 0x36, 0x1a, 0x77, 0x45, 0x12, 0x27,
    0x71, 0x36, 0x15, 0x80, 0xfe, 0x51, 0xd3, 0xa8,
    0x98, 0xc4, 0x6c, 0x20, 0x0d, 0x05, 0xd1, 0x33,
    0x68, 0xa0, 0xe9, 0xd5, 0x26, 0x75, 0x66, 0x7c,
    0x8a, 0x88, 0x4f, 0x9a, 0x34, 0x66, 0x74, 0x05,
    /* key_id = 0 */
    0x00,
    /* seq = 1 (LE uint32) */
    0x01, 0x00, 0x00, 0x00,
    /* program text: "x=1" */
    0x78, 0x3d, 0x31
};

/* VEC2: payload = sign(key_id=0|seq2|"y=2") | key_id=0 | seq2 | "y=2"  (72 bytes) */
static const uint8_t VEC2[] = {
    /* Ed25519 signature (64 bytes) over (key_id=0 | seq=2 LE | "y=2") */
    0x02, 0x5c, 0x49, 0xfc, 0xf9, 0xc5, 0xdd, 0x2c,
    0x0f, 0x36, 0xab, 0xfb, 0x55, 0xfb, 0x0d, 0xb0,
    0xbf, 0x51, 0x29, 0xb5, 0x1f, 0x86, 0x80, 0xcc,
    0x9e, 0x32, 0x47, 0xfb, 0xcb, 0xe7, 0x61, 0x9d,
    0xbf, 0x70, 0xc8, 0x20, 0x24, 0xa1, 0xd5, 0xf2,
    0x7c, 0xdb, 0x5e, 0xc0, 0x6c, 0xb7, 0xa6, 0x12,
    0x95, 0x87, 0x44, 0x93, 0xb6, 0x20, 0x17, 0x1a,
    0x2f, 0x06, 0xb4, 0xcf, 0x41, 0x0e, 0x1a, 0x0e,
    /* key_id = 0 */
    0x00,
    /* seq = 2 (LE uint32) */
    0x02, 0x00, 0x00, 0x00,
    /* program text: "y=2" */
    0x79, 0x3d, 0x32
};

/* VEC3: payload = sign(key1, key_id=1|seq1|"z=3") | key_id=1 | seq1 | "z=3"  (72 bytes) */
static const uint8_t VEC3[] = {
    /* Ed25519 signature (64 bytes) over (key_id=1 | seq=1 LE | "z=3") -- key 1 */
    0x78, 0x8c, 0x31, 0xe2, 0xec, 0xda, 0xb0, 0xca,
    0xc4, 0xa3, 0x41, 0xa9, 0x21, 0xcc, 0x88, 0xc2,
    0x05, 0xb2, 0xcf, 0xa0, 0xbc, 0x61, 0x2f, 0x7d,
    0x5f, 0x58, 0xb4, 0xfc, 0xea, 0x91, 0x1f, 0xf0,
    0x40, 0xcc, 0x15, 0xb6, 0x14, 0xbb, 0x38, 0x84,
    0x37, 0x70, 0xf7, 0x2e, 0x9c, 0x93, 0x23, 0x7f,
    0x26, 0x8a, 0xbb, 0xf3, 0xf8, 0x27, 0xbe, 0x26,
    0xee, 0x5d, 0xac, 0xc3, 0x30, 0xae, 0xed, 0x01,
    /* key_id = 1 */
    0x01,
    /* seq = 1 (LE uint32) */
    0x01, 0x00, 0x00, 0x00,
    /* program text: "z=3" */
    0x7a, 0x3d, 0x33
};

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 1 — Valid first payload (seq=1) is accepted
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_valid_first(void)
{
    printf("\n=== TEST 1: Valid first payload (key_id=0, seq=1) ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 0u;
    bool ok = rivr_ota_verify(VEC1, sizeof(VEC1));
    CHECK(ok,               "verify VEC1 (key_id=0, seq=1) returns true");
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

    /* seq bytes zeroed => sig mismatch AND anti-replay failure */
    s_stub_seq = 5u;
    uint8_t bad[sizeof(VEC1)];
    memcpy(bad, VEC1, sizeof(VEC1));
    /* Overwrite seq bytes to 0 (seq is now at [65..68] with key_id at [64]) */
    bad[65] = 0; bad[66] = 0; bad[67] = 0; bad[68] = 0;
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
    bad[65] = 0x99u;  /* corrupt seq[0] (now at offset 65, key_id at 64) */
    CHECK(!rivr_ota_verify(bad, sizeof(bad)),
          "tampered seq byte -> sig mismatch -> rejected");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 7 — Valid key_id=1 payload (VEC3) accepted via second key
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_second_key(void)
{
    printf("\n=== TEST 7: Valid key_id=1 payload (VEC3) ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 0u;
    bool ok = rivr_ota_verify(VEC3, sizeof(VEC3));
    CHECK(ok,               "verify VEC3 (key_id=1, seq=1) true");
    CHECK(s_stub_seq == 1u, "seq persisted as 1 after VEC3 accept");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 8 — Out-of-range key_id rejected
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_invalid_key_id(void)
{
    printf("\n=== TEST 8: Invalid key_id (out of range) ===\n");
#ifdef RIVR_SIGNED_PROG
    s_stub_seq = 0u;
    uint8_t bad[sizeof(VEC1)];
    memcpy(bad, VEC1, sizeof(VEC1));
    bad[64] = 0x02u;
    CHECK(!rivr_ota_verify(bad, sizeof(bad)), "key_id=2 (>=KEY_COUNT) rejected");
    bad[64] = 0xFFu;
    CHECK(!rivr_ota_verify(bad, sizeof(bad)), "key_id=0xFF rejected");
#else
    printf("  SKIP (unsigned build)\n");
#endif
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 9 — rivr_ota_activate() stores NUL-terminated text + sets pending
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_activate(void)
{
    printf("\n=== TEST 9: rivr_ota_activate() ===\n");
    s_nvs_store_called = false;
    s_stub_pending     = 0u;
    memset(s_nvs_program, 0, sizeof(s_nvs_program));

    bool ok = rivr_ota_activate(VEC1, sizeof(VEC1));
    CHECK(ok,                           "activate returns true");
    CHECK(s_nvs_store_called,           "nvs_store_program was called");
    CHECK(strcmp(s_nvs_program, "x=1") == 0,
                                        "stored program text == \"x=1\"");
    CHECK(s_stub_pending == 1u,         "ota_pending set to 1 after activate");

    /* Truncated payload -> activate should fail */
    ok = rivr_ota_activate(VEC1, RIVR_OTA_HDR_LEN);
    CHECK(!ok, "activate with payload == HDR_LEN -> false");
}

/* ══════════════════════════════════════════════════════════════════════════ *
 * TEST 10 — Boot confirm clears the ota_pending flag
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_boot_confirm(void)
{
    printf("\n=== TEST 10: Boot confirm / rollback flag ===\n");
    s_stub_pending = 1u;
    CHECK(rivr_ota_is_pending(),  "rivr_ota_is_pending() true after activate");

    bool ok = rivr_ota_confirm();
    CHECK(ok,                     "rivr_ota_confirm() returns true");
    CHECK(!rivr_ota_is_pending(), "rivr_ota_is_pending() false after confirm");
    CHECK(s_stub_pending == 0u,   "ota_pending NVS flag cleared to 0");

    /* Confirm is idempotent */
    ok = rivr_ota_confirm();
    CHECK(ok,                     "second rivr_ota_confirm() is idempotent");
    CHECK(!rivr_ota_is_pending(), "still not pending after second confirm");
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
    test_second_key();
    test_invalid_key_id();
    test_activate();
    test_boot_confirm();

    printf("\n%u PASS  %u FAIL\n", (unsigned)s_pass, (unsigned)s_fail);
    return (s_fail == 0u) ? 0 : 1;
}
