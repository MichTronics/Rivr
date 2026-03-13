/*
 * rivr_policy.c — Runtime-adjustable Rivr policy parameter implementation.
 *
 * See rivr_policy.h for full documentation.
 */

#include "rivr_policy.h"
#include "protocol.h"                         /* PKT_CHAT, PKT_DATA, et al.         */
#include "hal/feature_flags.h"               /* RIVR_FEATURE_SIGNED_PARAMS et al.  */
#include "rivr_programs/default_program.h"   /* RIVR_PARAM_* compiled-in defaults */
#include "beacon_sched.h"                     /* BEACON_POLL_MS                     */
#include "rivr_log.h"
#include "timebase.h"                         /* tb_millis() */
#include "crypto/hmac_sha256.h"               /* rivr_hmac_sha256()                */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* strtoul — rivr_policy_role_from_str */

#define TAG "RIVR_POLICY"

/* ── Global policy parameter state ──────────────────────────────────────── */

rivr_policy_params_t g_policy_params;

/* ── Policy metrics (private — exposed via rivr_policy_metrics_get()) ────── */

static rivr_policy_metrics_t s_policy_metrics;

/* ── Origination throttle timestamps (main-loop only, no ISR access) ─────── */

static uint32_t s_last_chat_orig_ms = 0u;  /**< Last allowed PKT_CHAT origination */
static uint32_t s_last_data_orig_ms = 0u;  /**< Last allowed PKT_DATA origination */

/* ── @PARAMS HMAC-SHA-256 key (32 bytes, initialised from RIVR_PARAMS_PSK_HEX) ── */

static uint8_t s_params_psk[32];   /**< Decoded PSK — zero-filled until init   */
static bool    s_params_psk_ready; /**< Set to true after first rivr_policy_init */

/** Decode RIVR_PARAMS_PSK_HEX at init time into s_params_psk[32]. */
static void s_psk_init(void)
{
    const char *hex = RIVR_PARAMS_PSK_HEX;
    for (uint8_t i = 0u; i < 32u; i++) {
        unsigned int b = 0u;
        /* sscanf is available (libc) and small — no heap use here */
        if (sscanf(hex + (size_t)i * 2u, "%02x", &b) != 1) { b = 0u; }
        s_params_psk[i] = (uint8_t)b;
    }
    s_params_psk_ready = true;
}

/* ── API ─────────────────────────────────────────────────────────────────── */

void rivr_policy_init(void)
{
    g_policy_params.beacon_interval_ms = RIVR_PARAM_BEACON_INTERVAL_MS;
    g_policy_params.beacon_jitter_ms   = RIVR_PARAM_BEACON_JITTER_MS;
    g_policy_params.chat_throttle_ms   = RIVR_PARAM_CHAT_THROTTLE_MS;
    g_policy_params.data_throttle_ms   = RIVR_PARAM_DATA_THROTTLE_MS;
    g_policy_params.duty_percent       = RIVR_PARAM_DUTY_PERCENT;
    g_policy_params.role               = (uint8_t)RIVR_NODE_ROLE_CLIENT;
    /* Zero all metrics counters on (re)init. */
    s_policy_metrics.params_update_count          = 0u;
    s_policy_metrics.last_params_update_uptime_ms = 0u;
    s_policy_metrics.policy_rebuild_count         = 0u;
    s_policy_metrics.policy_reload_count          = 0u;
    s_policy_metrics.duty_blocked_count           = 0u;
    s_policy_metrics.origination_drop_count       = 0u;
    s_policy_metrics.params_sig_ok_count          = 0u;
    s_policy_metrics.params_sig_fail_count        = 0u;
    s_policy_metrics.beacon_config_rejected_total = 0u;
    s_last_chat_orig_ms = 0u;
    s_last_data_orig_ms = 0u;
    s_psk_init();   /* decode RIVR_PARAMS_PSK_HEX once */
    RIVR_LOGI(TAG, "policy init: beacon=%lums jitter=%lums chat=%lums data=%lums duty=%u%%",
              (unsigned long)g_policy_params.beacon_interval_ms,
              (unsigned long)g_policy_params.beacon_jitter_ms,
              (unsigned long)g_policy_params.chat_throttle_ms,
              (unsigned long)g_policy_params.data_throttle_ms,
              (unsigned)g_policy_params.duty_percent);
}

void rivr_policy_set_param(uint8_t param_id, uint32_t value)
{
    switch ((rivr_param_id_t)param_id) {
        case RIVR_PARAM_ID_BEACON_INTERVAL:
            /* Hard minimum: 60 000 ms (1 minute).
             * Values below this create unsustainable beacon rates in dense
             * EU868 networks and are REJECTED; the previous valid value is
             * preserved.  beacon_config_rejected_total is incremented so
             * operators can detect misconfiguration via @POLICY output. */
            if (value < 60000u) {
                s_policy_metrics.beacon_config_rejected_total++;
                RIVR_LOGW(TAG,
                          "beacon_interval_ms=%lu rejected: below hard minimum 60000 ms"
                          " (EU868 safety floor); previous value %lu ms kept",
                          (unsigned long)value,
                          (unsigned long)g_policy_params.beacon_interval_ms);
                return;
            }
            g_policy_params.beacon_interval_ms = value;
            RIVR_LOGI(TAG, "param update: beacon_interval_ms=%lu", (unsigned long)value);
            s_policy_metrics.params_update_count++;
            s_policy_metrics.last_params_update_uptime_ms = tb_millis();
            break;

        case RIVR_PARAM_ID_BEACON_JITTER:
            /* Max 600 000 ms (10 min) — jitter larger than the minimum
             * interval makes no practical sense and is rejected. */
            if (value > 600000u) {
                RIVR_LOGW(TAG,
                          "beacon_jitter_ms=%lu too large (max 600000), ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.beacon_jitter_ms = value;
            RIVR_LOGI(TAG, "param update: beacon_jitter_ms=%lu", (unsigned long)value);
            s_policy_metrics.params_update_count++;
            s_policy_metrics.last_params_update_uptime_ms = tb_millis();
            break;

        case RIVR_PARAM_ID_CHAT_THROTTLE:
            if (value < 100u) {
                RIVR_LOGW(TAG, "chat_throttle_ms=%lu too small (min 100), ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.chat_throttle_ms = value;
            RIVR_LOGI(TAG, "param update: chat_throttle_ms=%lu", (unsigned long)value);
            s_policy_metrics.params_update_count++;
            s_policy_metrics.last_params_update_uptime_ms = tb_millis();
            break;

        case RIVR_PARAM_ID_DATA_THROTTLE:
            if (value < 100u) {
                RIVR_LOGW(TAG, "data_throttle_ms=%lu too small (min 100), ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.data_throttle_ms = value;
            RIVR_LOGI(TAG, "param update: data_throttle_ms=%lu", (unsigned long)value);
            s_policy_metrics.params_update_count++;
            s_policy_metrics.last_params_update_uptime_ms = tb_millis();
            break;

        case RIVR_PARAM_ID_DUTY_PERCENT:
            if (value < 1u || value > 10u) {
                RIVR_LOGW(TAG, "duty_percent=%lu out of range [1..10], ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.duty_percent = (uint8_t)value;
            RIVR_LOGI(TAG, "param update: duty_percent=%u", (unsigned)value);
            s_policy_metrics.params_update_count++;
            s_policy_metrics.last_params_update_uptime_ms = tb_millis();
            break;

        case RIVR_PARAM_ID_ROLE:
            if (value < 1u || value > 3u) {
                RIVR_LOGW(TAG, "role=%lu out of range [1..3], ignored", (unsigned long)value);
                return;
            }
            g_policy_params.role = (uint8_t)value;
            RIVR_LOGI(TAG, "param update: role=%s", rivr_policy_role_name(g_policy_params.role));
            s_policy_metrics.params_update_count++;
            s_policy_metrics.last_params_update_uptime_ms = tb_millis();
            break;

        default:
            RIVR_LOGW(TAG, "unknown param_id=%u, ignored", (unsigned)param_id);
            break;
    }
}

/* ── Role helpers ──────────────────────────────────────────────────────── */

uint8_t rivr_policy_role_from_str(const char *s)
{
    if (!s || *s == '\0') return (uint8_t)RIVR_NODE_ROLE_CLIENT;
    if (strcmp(s, "client")   == 0) return (uint8_t)RIVR_NODE_ROLE_CLIENT;
    if (strcmp(s, "repeater") == 0) return (uint8_t)RIVR_NODE_ROLE_REPEATER;
    if (strcmp(s, "gateway")  == 0) return (uint8_t)RIVR_NODE_ROLE_GATEWAY;
    /* Numeric fallback: "1", "2", "3" */
    unsigned long v = strtoul(s, NULL, 10);
    if (v >= 1u && v <= 3u) return (uint8_t)v;
    return (uint8_t)RIVR_NODE_ROLE_CLIENT;
}

const char *rivr_policy_role_name(uint8_t role)
{
    switch ((rivr_node_role_t)role) {
        case RIVR_NODE_ROLE_CLIENT:   return "client";
        case RIVR_NODE_ROLE_REPEATER: return "repeater";
        case RIVR_NODE_ROLE_GATEWAY:  return "gateway";
        default:                      return "?";
    }
}

/* ── Origination gate ────────────────────────────────────────────────────── */

bool rivr_policy_allow_origination(uint8_t pkt_type, uint32_t now_ms)
{
    uint32_t throttle_ms;
    uint32_t *last_ms;

    switch (pkt_type) {
        case PKT_CHAT:
            throttle_ms = g_policy_params.chat_throttle_ms;
            last_ms     = &s_last_chat_orig_ms;
            break;
        case PKT_DATA:
            throttle_ms = g_policy_params.data_throttle_ms;
            last_ms     = &s_last_data_orig_ms;
            break;
        default:
            /* Unknown types are not throttled. */
            return true;
    }

    /* Allow if no packet has ever been sent, or if the window has elapsed. */
    if (*last_ms == 0u || (now_ms - *last_ms) >= throttle_ms) {
        *last_ms = now_ms;
        return true;
    }

    /* Throttled — increment counter and deny. */
    s_policy_metrics.origination_drop_count++;
    RIVR_LOGD(TAG, "origination throttled: pkt_type=%u drops=%lu",
              (unsigned)pkt_type,
              (unsigned long)s_policy_metrics.origination_drop_count);
    return false;
}

/* ── @PARAMS signature verification ────────────────────────────────────── */

bool rivr_verify_params_sig(const char *buf, size_t len)
{
#if !RIVR_FEATURE_SIGNED_PARAMS
    /* Signing disabled — accept everything unconditionally.                 */
    (void)buf; (void)len;
    return true;

#else /* RIVR_FEATURE_SIGNED_PARAMS == 1 */

    /* Locate the " sig=" token that marks the boundary of signed content.  */
    const char *sig_field = strstr(buf, " sig=");

    if (!sig_field) {
        /* No signature present. */
#  if RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS
        /* Dev grace: silently accept, don't touch counters. */
        RIVR_LOGD(TAG, "verify_params_sig: no sig= — accepted (allow-unsigned)");
        return true;
#  else
        /* Production: reject unsigned. */
        s_policy_metrics.params_sig_fail_count++;
        RIVR_LOGW(TAG, "verify_params_sig: no sig= — REJECTED (allow-unsigned=0)");
        return false;
#  endif
    }

    /* Signed content = everything before " sig=". */
    size_t msg_len = (size_t)(sig_field - buf);

    /* Hex-encoded HMAC-SHA-256 = 64 hex chars (32 bytes).                  */
    const char *hex = sig_field + 5u;   /* skip " sig=" (5 chars)           */

    /* Require exactly 64 hex nibbles, optionally followed by NUL or space. */
    size_t hex_avail = 0u;
    while (hex[hex_avail] && hex[hex_avail] != ' ' && hex[hex_avail] != '\t'
           && hex[hex_avail] != '\r' && hex[hex_avail] != '\n') {
        hex_avail++;
    }
    if (hex_avail != 64u) {
        s_policy_metrics.params_sig_fail_count++;
        RIVR_LOGW(TAG, "verify_params_sig: sig= length %u != 64 — REJECTED",
                  (unsigned)hex_avail);
        return false;
    }

    /* Decode 32-byte received MAC from hex. */
    uint8_t received_mac[32];
    for (uint8_t i = 0u; i < 32u; i++) {
        unsigned int bv = 0u;
        if (sscanf(hex + (size_t)i * 2u, "%02x", &bv) != 1) {
            s_policy_metrics.params_sig_fail_count++;
            RIVR_LOGW(TAG, "verify_params_sig: hex decode error at byte %u", (unsigned)i);
            return false;
        }
        received_mac[i] = (uint8_t)bv;
    }

    /* Compute expected HMAC-SHA-256 over the signed content. */
    if (!s_params_psk_ready) { s_psk_init(); }
    uint8_t expected_mac[32];
    rivr_hmac_sha256(s_params_psk, sizeof(s_params_psk),
                     (const uint8_t *)buf, msg_len,
                     expected_mac);

    /* Constant-time compare (prevents timing oracle). */
    uint8_t diff = 0u;
    for (uint8_t i = 0u; i < 32u; i++) {
        diff |= expected_mac[i] ^ received_mac[i];
    }

    if (diff != 0u) {
        s_policy_metrics.params_sig_fail_count++;
        RIVR_LOGW(TAG, "verify_params_sig: MAC mismatch — REJECTED");
        return false;
    }

    s_policy_metrics.params_sig_ok_count++;
    RIVR_LOGI(TAG, "verify_params_sig: sig OK (ok=%lu fail=%lu)",
              (unsigned long)s_policy_metrics.params_sig_ok_count,
              (unsigned long)s_policy_metrics.params_sig_fail_count);
    return true;

#endif /* RIVR_FEATURE_SIGNED_PARAMS */
}

void rivr_policy_build_program(char *buf, size_t bufsz)
{
    /*
     * Role-adjusted effective throttle values:
     *   CLIENT   — use params as-is (standard relay rate).
     *   REPEATER/GATEWAY — halve the throttle window (floor 100 ms)
     *     to allow higher relay throughput within the same duty budget.
     *
     * Generated program shape (RIVR_MESH_PROGRAM with runtime params):
     *
     *   source rf_rx @lmp = rf;
     *   source beacon_tick = timer(<beacon_interval_ms>);
     *
     *   let chat = rf_rx
     *     |> filter.pkt_type(1)
     *     |> budget.toa_us(280000, 0.<duty_percent:02d>, 280000)
     *     |> throttle.ms(<eff_chat_throttle_ms>);
     *
     *   let data = rf_rx
     *     |> filter.pkt_type(6)
     *     |> throttle.ms(<eff_data_throttle_ms>);
     *
     *   emit { io.lora.beacon(beacon_tick); }
     *
     * Note: io.lora.tx is intentionally absent. Relay (hop increment, TTL
     * decrement, jitter) is handled exclusively by the C-layer (maybe_relay).
     * budget.toa_us second arg uses 0.<duty_percent_2digit> format so that
     * duty=1 → "0.01", duty=10 → "0.10" (RIVR parser accepts plain decimal).
     *
     * Max generated length (all params at u32 max, duty=10): ~340 chars.
     * bufsz must be >= 512. Truncation is detected and handled below.
     */
    uint32_t eff_chat = g_policy_params.chat_throttle_ms;
    uint32_t eff_data = g_policy_params.data_throttle_ms;
    if (g_policy_params.role == (uint8_t)RIVR_NODE_ROLE_REPEATER ||
        g_policy_params.role == (uint8_t)RIVR_NODE_ROLE_GATEWAY) {
        eff_chat = (eff_chat / 2u < 100u) ? 100u : eff_chat / 2u;
        eff_data = (eff_data / 2u < 100u) ? 100u : eff_data / 2u;
    }

    int len = snprintf(buf, bufsz,
        "source rf_rx @lmp = rf;\n"
        "source beacon_tick = timer(%lu);\n"
        "\n"
        "let chat = rf_rx\n"
        "  |> filter.pkt_type(1)\n"
        "  |> budget.toa_us(280000, 0.%02u, 280000)\n"
        "  |> throttle.ms(%lu);\n"
        "\n"
        "let data = rf_rx\n"
        "  |> filter.pkt_type(6)\n"
        "  |> throttle.ms(%lu);\n"
        "\n"
        "emit { io.lora.beacon(beacon_tick); }\n",
        (unsigned long)BEACON_POLL_MS,   /* always 60 000 ms poll; C gate controls TX rate */
        (unsigned)g_policy_params.duty_percent,
        (unsigned long)eff_chat,
        (unsigned long)eff_data);

    /* Guarantee NUL termination regardless of snprintf outcome. */
    buf[bufsz - 1u] = '\0';

    if (len < 0) {
        /* Encoding error (should never happen with %lu/%u format). */
        RIVR_LOGE(TAG, "rivr_policy_build_program: snprintf encoding error");
        buf[0] = '\0';
    } else if ((size_t)len >= bufsz) {
        /* Output was truncated — bufsz is too small. This should not happen
         * with bufsz=512 and validated param values, but guard defensively. */
        RIVR_LOGE(TAG, "rivr_policy_build_program: output truncated "
                  "(%d chars needed, buf=%u) — using empty program",
                  len, (unsigned)bufsz);
        buf[0] = '\0';
    } else {
        /* Success — count the build. */
        s_policy_metrics.policy_rebuild_count++;
    }
}

/* ── Getters ─────────────────────────────────────────────────────────────── */

const rivr_policy_params_t *rivr_policy_params_get(void)
{
    return &g_policy_params;
}

const rivr_policy_metrics_t *rivr_policy_metrics_get(void)
{
    return &s_policy_metrics;
}

/* ── Reload hook (called from rivr_embed.c after successful hot-reload) ──── */

void rivr_policy_notify_reload(void)
{
    s_policy_metrics.policy_reload_count++;
    RIVR_LOGI(TAG, "policy_reload_count=%lu",
              (unsigned long)s_policy_metrics.policy_reload_count);
}

/* ── @POLICY print ───────────────────────────────────────────────────────── */

void rivr_policy_print(void)
{
    printf("@POLICY {"
           "\"beacon\":%lu,"
           "\"jitter\":%lu,"
           "\"chat\":%lu,"
           "\"data\":%lu,"
           "\"duty\":%u,"
           "\"role\":\"%s\","
           "\"updates\":%lu,"
           "\"last_update_ms\":%lu,"
           "\"rebuilds\":%lu,"
           "\"reloads\":%lu,"
           "\"duty_blocked\":%lu,"
           "\"orig_drops\":%lu,"
           "\"sig_ok\":%lu,"
           "\"sig_fail\":%lu,"
           "\"bcn_rejected\":%lu"
           "}\r\n",
           (unsigned long)g_policy_params.beacon_interval_ms,
           (unsigned long)g_policy_params.beacon_jitter_ms,
           (unsigned long)g_policy_params.chat_throttle_ms,
           (unsigned long)g_policy_params.data_throttle_ms,
           (unsigned)g_policy_params.duty_percent,
           rivr_policy_role_name(g_policy_params.role),
           (unsigned long)s_policy_metrics.params_update_count,
           (unsigned long)s_policy_metrics.last_params_update_uptime_ms,
           (unsigned long)s_policy_metrics.policy_rebuild_count,
           (unsigned long)s_policy_metrics.policy_reload_count,
           (unsigned long)s_policy_metrics.duty_blocked_count,
           (unsigned long)s_policy_metrics.origination_drop_count,
           (unsigned long)s_policy_metrics.params_sig_ok_count,
           (unsigned long)s_policy_metrics.params_sig_fail_count,
           (unsigned long)s_policy_metrics.beacon_config_rejected_total);
    fflush(stdout);
}

/* ── Self-test ───────────────────────────────────────────────────────────── */

#ifdef RIVR_POLICY_SELFTEST
#include <assert.h>

void rivr_policy_selftest(void)
{
    rivr_policy_init();
    const rivr_policy_metrics_t *m = rivr_policy_metrics_get();

    /* Valid beacon interval → counter increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_INTERVAL, 60000u);
    assert(m->params_update_count == 1u);
    assert(m->beacon_config_rejected_total == 0u);

    /* Invalid beacon interval: below hard minimum 60 000 ms → must NOT increment,
     * but beacon_config_rejected_total MUST increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_INTERVAL, 59999u);
    assert(m->params_update_count == 1u);
    assert(m->beacon_config_rejected_total == 1u);
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_INTERVAL, 500u);
    assert(m->params_update_count == 1u);
    assert(m->beacon_config_rejected_total == 2u);

    /* Valid jitter param (0 → no jitter) → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_JITTER, 0u);
    assert(m->params_update_count == 2u);
    /* Invalid jitter (> 600 000) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_JITTER, 600001u);
    assert(m->params_update_count == 2u);
    /* Valid jitter → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_JITTER, 120000u);
    assert(m->params_update_count == 3u);
    assert(g_policy_params.beacon_jitter_ms == 120000u);

    /* Valid chat throttle → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, 500u);
    assert(m->params_update_count == 4u);

    /* Invalid chat throttle (below 100 ms) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, 50u);
    assert(m->params_update_count == 4u);

    /* Valid data throttle → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_DATA_THROTTLE, 1000u);
    assert(m->params_update_count == 5u);

    /* Valid duty percent → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, 5u);
    assert(m->params_update_count == 6u);

    /* Invalid duty (out of [1..10]) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, 11u);
    assert(m->params_update_count == 6u);
    rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, 0u);
    assert(m->params_update_count == 6u);

    /* Unknown param_id → no increment. */
    rivr_policy_set_param(99u, 1000u);
    assert(m->params_update_count == 6u);

    /* ── Role parameter tests ── */

    /* Valid role (repeater) → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_ROLE, (uint32_t)RIVR_NODE_ROLE_REPEATER);
    assert(m->params_update_count == 7u);
    assert(g_policy_params.role == (uint8_t)RIVR_NODE_ROLE_REPEATER);

    /* Invalid role (out of [1..3]) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_ROLE, 99u);
    assert(m->params_update_count == 7u);
    rivr_policy_set_param(RIVR_PARAM_ID_ROLE, 0u);
    assert(m->params_update_count == 7u);

    /* Role-based program generation: repeater throttle < client throttle */
    {
        char prog_client[512];
        char prog_repeater[512];
        char prog_gateway[512];

        /* Reset to client with known throttle values */
        rivr_policy_init();
        rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, 2000u);
        rivr_policy_set_param(RIVR_PARAM_ID_DATA_THROTTLE, 2000u);
        rivr_policy_build_program(prog_client, sizeof(prog_client));

        /* Repeater: throttle halved → different program */
        rivr_policy_set_param(RIVR_PARAM_ID_ROLE, (uint32_t)RIVR_NODE_ROLE_REPEATER);
        rivr_policy_build_program(prog_repeater, sizeof(prog_repeater));
        assert(strcmp(prog_client, prog_repeater) != 0);

        /* Gateway produces same result as repeater */
        rivr_policy_set_param(RIVR_PARAM_ID_ROLE, (uint32_t)RIVR_NODE_ROLE_GATEWAY);
        rivr_policy_build_program(prog_gateway, sizeof(prog_gateway));
        assert(strcmp(prog_repeater, prog_gateway) == 0);

        /* Restore to client → program matches original */
        rivr_policy_set_param(RIVR_PARAM_ID_ROLE, (uint32_t)RIVR_NODE_ROLE_CLIENT);
        char prog_client2[512];
        rivr_policy_build_program(prog_client2, sizeof(prog_client2));
        assert(strcmp(prog_client, prog_client2) == 0);
    }

    /* rivr_policy_role_from_str: named strings */
    assert(rivr_policy_role_from_str("client")   == (uint8_t)RIVR_NODE_ROLE_CLIENT);
    assert(rivr_policy_role_from_str("repeater") == (uint8_t)RIVR_NODE_ROLE_REPEATER);
    assert(rivr_policy_role_from_str("gateway")  == (uint8_t)RIVR_NODE_ROLE_GATEWAY);
    assert(rivr_policy_role_from_str("1")        == (uint8_t)RIVR_NODE_ROLE_CLIENT);
    assert(rivr_policy_role_from_str("2")        == (uint8_t)RIVR_NODE_ROLE_REPEATER);
    assert(rivr_policy_role_from_str("junk")     == (uint8_t)RIVR_NODE_ROLE_CLIENT);
    assert(rivr_policy_role_from_str(NULL)       == (uint8_t)RIVR_NODE_ROLE_CLIENT);

    /* ── Origination gate tests ── */
    {
        rivr_policy_init();   /* resets timestamps and drops counter */
        rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, 500u);
        rivr_policy_set_param(RIVR_PARAM_ID_DATA_THROTTLE, 500u);
        const rivr_policy_metrics_t *om = rivr_policy_metrics_get();

        /* First call at t=0 → always passes. */
        assert(rivr_policy_allow_origination(1u, 100u) == true);
        assert(om->origination_drop_count == 0u);

        /* Second call within throttle window (t=100+100=200 < 100+500) → drops. */
        assert(rivr_policy_allow_origination(1u, 200u) == false);
        assert(om->origination_drop_count == 1u);

        /* Call after the window elapses → passes again. */
        assert(rivr_policy_allow_origination(1u, 700u) == true);
        assert(om->origination_drop_count == 1u);

        /* PKT_DATA uses its own independent timestamp. */
        assert(rivr_policy_allow_origination(6u, 800u) == true);
        assert(rivr_policy_allow_origination(6u, 850u) == false);
        assert(om->origination_drop_count == 2u);

        /* Unknown pkt_type always passes without touching the counter. */
        assert(rivr_policy_allow_origination(99u, 1u) == true);
        assert(om->origination_drop_count == 2u);
    }

    /* ── HMAC-SHA-256 test vector (RFC 4231, Test Case 1) ── *
     *
     *  Key  = 20 × 0x0b                                     *
     *  Data = "Hi There"                                     *
     *  Expected (hex):                                        *
     *    b0344c61 d8db3853 5ca8afce af0bf12b                 *
     *    881dc200 c9833da7 26e9376c 2e32cff7                 *
     *                                                         *
     * This validates the raw HMAC implementation independent  *
     * of the RIVR_FEATURE_SIGNED_PARAMS build flag.          *
     * ─────────────────────────────────────────────────────── */
    {
        static const uint8_t tc1_key[20] = {
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
        };
        static const uint8_t tc1_data[] = "Hi There";
        static const uint8_t tc1_exp[32] = {
            0xb0,0x34,0x4c,0x61, 0xd8,0xdb,0x38,0x53,
            0x5c,0xa8,0xaf,0xce, 0xaf,0x0b,0xf1,0x2b,
            0x88,0x1d,0xc2,0x00, 0xc9,0x83,0x3d,0xa7,
            0x26,0xe9,0x37,0x6c, 0x2e,0x32,0xcf,0xf7
        };
        uint8_t got[32];
        rivr_hmac_sha256(tc1_key, sizeof(tc1_key),
                         tc1_data, 8u,   /* "Hi There" = 8 bytes */
                         got);
        assert(memcmp(got, tc1_exp, 32u) == 0);
        RIVR_LOGI(TAG, "SELFTEST: HMAC-SHA-256 RFC-4231 TC1 OK");
    }

    /* ── @PARAMS signature round-trip (FEATURE_SIGNED_PARAMS-gated path) ── *
     *                                                                        *
     * Whether RIVR_FEATURE_SIGNED_PARAMS is 0 or 1, we test the low-level  *
     * HMAC path directly so the test is always meaningful.  The             *
     * rivr_verify_params_sig() assertions are additionally protected by a  *
     * #if so they match the active flag combination.                        *
     * ─────────────────────────────────────────────────────────────────────  */
    {
        rivr_policy_init();   /* resets PSK + counters */
        const rivr_policy_metrics_t *sm = rivr_policy_metrics_get();

        /* Build a params string, sign it with the compiled-in PSK. */
        const char *test_content = "@PARAMS beacon=300000 chat=2000";
        uint8_t test_mac[32];
        rivr_hmac_sha256(s_params_psk, sizeof(s_params_psk),
                         (const uint8_t *)test_content, strlen(test_content),
                         test_mac);

        /* Hex-encode the MAC. */
        char test_sig_hex[65];
        for (uint8_t ci = 0u; ci < 32u; ci++) {
            (void)snprintf(test_sig_hex + (size_t)ci * 2u, 3u,
                           "%02x", (unsigned)test_mac[ci]);
        }

        /* Build full @PARAMS string: content + " sig=" + hex. */
        char full_params[256];
        (void)snprintf(full_params, sizeof(full_params),
                       "%s sig=%s", test_content, test_sig_hex);

        /* Round-trip: verify must accept this. */
        assert(rivr_verify_params_sig(full_params, strlen(full_params)) == true);

        /* Confirm counters: with SIGNED_PARAMS=0, counters stay at 0.
         * With SIGNED_PARAMS=1, sig_ok increments. */
#if RIVR_FEATURE_SIGNED_PARAMS
        assert(sm->params_sig_ok_count == 1u);
        assert(sm->params_sig_fail_count == 0u);
#else
        assert(sm->params_sig_ok_count == 0u);
        assert(sm->params_sig_fail_count == 0u);
#endif

        /* Tamper: change one byte of signed content → must reject.
         * When SIGNED_PARAMS=0, the stub still accepts — guard accordingly. */
        char tampered[256];
        memcpy(tampered, full_params, strlen(full_params) + 1u);
        tampered[10] ^= 0x01u;   /* flip one bit in the params payload */
#if RIVR_FEATURE_SIGNED_PARAMS
        assert(rivr_verify_params_sig(tampered, strlen(tampered)) == false);
        assert(sm->params_sig_fail_count == 1u);
#endif

        /* Unsigned @PARAMS: accepted or rejected based on ALLOW_UNSIGNED. */
        const char *unsigned_params = "@PARAMS beacon=600000";
#if RIVR_FEATURE_SIGNED_PARAMS && !RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS
        assert(rivr_verify_params_sig(unsigned_params,
                                      strlen(unsigned_params)) == false);
#else
        assert(rivr_verify_params_sig(unsigned_params,
                                      strlen(unsigned_params)) == true);
#endif

        RIVR_LOGI(TAG, "SELFTEST: @PARAMS sig round-trip OK "
                  "(SIGNED_PARAMS=%d ALLOW_UNSIGNED=%d)",
                  RIVR_FEATURE_SIGNED_PARAMS,
                  RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS);
    }

    RIVR_LOGI(TAG, "RIVR_POLICY_SELFTEST: all assertions passed "
              "(params+role+origination+hmac+sig)");
}

#endif /* RIVR_POLICY_SELFTEST */
