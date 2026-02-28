/*
 * rivr_ota.c — Signed OTA RIVR program update gate.
 *
 * See rivr_ota.h for the full wire-format and API description.
 */

#include "rivr_ota.h"
#include "rivr_pubkey.h"
#include "ed25519_verify.h"

/* rivr_nvs_store_program() is declared in rivr_embed.h (ESP-IDF layer).
 * We reach it through a forward declaration here so this translation unit
 * remains independent of the full ESP-IDF headers when compiled in the
 * host-side unit-test harness (tests/test_ota.c supplies its own stub).  */
extern bool rivr_nvs_store_program(const char *src);

/* ── NVS anti-replay persistence ────────────────────────────────────────── */
/*
 * Platform glue: on ESP-IDF we use NVS directly.  Under the test harness the
 * linker pulls in the stubs from tests/test_stubs.c.
 */

#ifdef ESP_IDF_VERSION   /* real firmware build */
#  include "nvs_flash.h"
#  include "nvs.h"
#  define NVS_NS  "rivr"
#  define NVS_KEY "ota_seq"

static uint32_t load_last_seq(void)
{
    nvs_handle_t h;
    uint32_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY, &val);
        nvs_close(h);
    }
    return val;
}

static bool save_last_seq(uint32_t seq)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_u32(h, NVS_KEY, seq) == ESP_OK)
           && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

#else   /* host / unit-test build — stubs provided by tests/test_stubs.c */
extern uint32_t ota_stub_load_seq(void);
extern bool     ota_stub_save_seq(uint32_t seq);
static uint32_t load_last_seq(void)      { return ota_stub_load_seq(); }
static bool     save_last_seq(uint32_t s){ return ota_stub_save_seq(s); }
#endif

/* ── Public API ─────────────────────────────────────────────────────────── */

bool rivr_ota_verify(const uint8_t *payload, size_t payload_len)
{
#ifndef RIVR_SIGNED_PROG
    /* Unsigned build: accept everything */
    (void)payload; (void)payload_len;
    return true;
#else
    if (!payload || payload_len <= RIVR_OTA_HDR_LEN) return false;

    /* Split payload: sig | seq_bytes | program_text */
    const uint8_t *sig      = payload;
    const uint8_t *seq_bytes = payload + RIVR_OTA_SIG_LEN;        /* 4 bytes */
    const uint8_t *msg       = payload + RIVR_OTA_HDR_LEN;         /* text   */
    size_t         msg_len   = payload_len - RIVR_OTA_HDR_LEN;

    /* The signed message is (seq_bytes ‖ program_text) so the seq is covered
     * by the signature and cannot be tampered with independently.            */

    /* 1. Signature check: sig over (seq ‖ text) using the device public key */
    /* Build the full message buffer seq ‖ text for verification */
    /* We pass it as two parts via a small stack-allocated concat buffer.
     * payload_len is bounded by PKT_PROG_PUSH's uint8_t payload_len field
     * (max 255), so the full signing input is at most 255 - 64 = 191 bytes,
     * well within the 256-byte VLA below.                                    */
    uint8_t signed_msg[256];
    if (payload_len - RIVR_OTA_SIG_LEN > sizeof(signed_msg)) return false;
    size_t signed_len = payload_len - RIVR_OTA_SIG_LEN;
    /* seq_bytes + text are contiguous directly after sig: */
    __builtin_memcpy(signed_msg, seq_bytes, signed_len);

    if (!ed25519_verify_detached(sig, signed_msg, signed_len,
                                 RIVR_OTA_PUBLIC_KEY)) {
        return false;
    }

    /* 2. Anti-replay: seq must be strictly greater than last accepted */
    uint32_t seq = (uint32_t)seq_bytes[0]
                 | ((uint32_t)seq_bytes[1] << 8)
                 | ((uint32_t)seq_bytes[2] << 16)
                 | ((uint32_t)seq_bytes[3] << 24);

    uint32_t last = load_last_seq();
    if (seq <= last) return false;

    /* 3. Persist the new seq so reboots don't allow replay */
    if (!save_last_seq(seq)) return false;

    (void)msg; (void)msg_len;
    return true;
#endif /* RIVR_SIGNED_PROG */
}

bool rivr_ota_activate(const uint8_t *payload, size_t payload_len)
{
    if (!payload || payload_len <= RIVR_OTA_HDR_LEN) return false;

    /* Program text starts right after the header */
    const char *text    = (const char *)(payload + RIVR_OTA_HDR_LEN);
    size_t      text_len = payload_len - RIVR_OTA_HDR_LEN;

    /* rivr_nvs_store_program expects a NUL-terminated string.  The caller
     * (rivr_sources.c) already NUL-terminates prog_buf, but the text pointer
     * here points into the raw radio payload.  Use a small stack copy.       */
    char buf[256];
    if (text_len >= sizeof(buf)) text_len = sizeof(buf) - 1;
    __builtin_memcpy(buf, text, text_len);
    buf[text_len] = '\0';

    return rivr_nvs_store_program(buf);
}
