/*
 * rivr_ota.c — LEGACY FILE: no longer compiled.
 *
 * The OTA implementation has been split into two translation units:
 *
 *   firmware_core/rivr_ota_core.c      — pure logic, host-safe, no ESP-IDF
 *   firmware_core/rivr_ota_platform.c  — ESP-IDF NVS backend
 *
 * Update firmware_core/CMakeLists.txt references both new files.
 * Host unit tests (tests/test_ota.c) link only rivr_ota_core.c and provide
 * platform-interface stubs directly.
 *
 * See firmware_core/rivr_ota_platform.h for the platform interface design.
 *
 * This file is intentionally left as a comment-only marker so that any
 * out-of-tree branch that still references it gets a clear error message
 * rather than a silent ODR violation.
 */

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

#if !RIVR_SIM_MODE   /* real firmware build */
#  include "nvs_flash.h"
#  include "nvs.h"
#  define NVS_NS      "rivr"
#  define NVS_KEY_SEQ "ota_seq"
#  define NVS_KEY_PND "ota_pending"

static uint32_t __attribute__((unused)) load_last_seq(void)
{
    nvs_handle_t h;
    uint32_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_SEQ, &val);
        nvs_close(h);
    }
    return val;
}

static bool __attribute__((unused)) save_last_seq(uint32_t seq)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_u32(h, NVS_KEY_SEQ, seq) == ESP_OK)
           && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

static uint32_t load_ota_pending(void)
{
    nvs_handle_t h;
    uint32_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_PND, &val);
        nvs_close(h);
    }
    return val;
}

static bool save_ota_pending(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_u32(h, NVS_KEY_PND, v) == ESP_OK)
           && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

#else   /* host / unit-test / sim build — stubs provided by tests/test_stubs.c */
extern uint32_t ota_stub_load_seq(void);
extern bool     ota_stub_save_seq(uint32_t seq);
extern uint32_t ota_stub_load_pending(void);
extern bool     ota_stub_save_pending(uint32_t v);
static uint32_t __attribute__((unused)) load_last_seq(void)       { return ota_stub_load_seq(); }
static bool     __attribute__((unused)) save_last_seq(uint32_t s) { return ota_stub_save_seq(s); }
static uint32_t load_ota_pending(void)         { return ota_stub_load_pending(); }
static bool     save_ota_pending(uint32_t v)   { return ota_stub_save_pending(v); }
#endif

/* ── Public API ─────────────────────────────────────────────────────────── */

bool rivr_ota_verify(const uint8_t *payload, size_t payload_len)
{
#if !RIVR_SIGNED_PROG
    /* Unsigned build: accept everything */
    (void)payload; (void)payload_len;
    return true;
#else
    if (!payload || payload_len <= RIVR_OTA_HDR_LEN) return false;

    /* Split payload: sig | key_id | seq_bytes | program_text */
    const uint8_t *sig       = payload;
    uint8_t        key_id    = payload[RIVR_OTA_SIG_LEN];
    const uint8_t *seq_bytes = payload + RIVR_OTA_SIG_LEN + RIVR_OTA_KEY_LEN;

    /* Reject out-of-range key_id before any crypto work */
    if (key_id >= RIVR_OTA_KEY_COUNT) return false;
    const uint8_t *pub_key = RIVR_OTA_KEYS[key_id];

    /* The signed message is (key_id ‖ seq_bytes ‖ program_text): everything
     * after the 64-byte signature.  key_id is inside the coverage so an
     * attacker cannot substitute a different key_id post-hoc.               */
    uint8_t signed_msg[256];
    if (payload_len - RIVR_OTA_SIG_LEN > sizeof(signed_msg)) return false;
    size_t signed_len = payload_len - RIVR_OTA_SIG_LEN;
    __builtin_memcpy(signed_msg, payload + RIVR_OTA_SIG_LEN, signed_len);

    if (!ed25519_verify_detached(sig, signed_msg, signed_len, pub_key)) {
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

    return true;
#endif /* !RIVR_SIGNED_PROG */
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

    if (!rivr_nvs_store_program(buf)) return false;

    /* Mark the update as pending-confirm so a rollback can occur if the
     * new program crashes before rivr_ota_confirm() is called.              */
    (void)save_ota_pending(1u);
    return true;
}

bool rivr_ota_confirm(void)
{
    return save_ota_pending(0u);
}

bool rivr_ota_is_pending(void)
{
    return load_ota_pending() != 0u;
}
