/*
 * rivr_policy.c — Runtime-adjustable Rivr policy parameter implementation.
 *
 * See rivr_policy.h for full documentation.
 */

#include "rivr_policy.h"
#include "rivr_programs/default_program.h"   /* RIVR_PARAM_* compiled-in defaults */
#include "rivr_log.h"
#include "timebase.h"                         /* tb_millis() */
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

/* ── API ─────────────────────────────────────────────────────────────────── */

void rivr_policy_init(void)
{
    g_policy_params.beacon_interval_ms = RIVR_PARAM_BEACON_INTERVAL_MS;
    g_policy_params.chat_throttle_ms   = RIVR_PARAM_CHAT_THROTTLE_MS;
    g_policy_params.data_throttle_ms   = RIVR_PARAM_DATA_THROTTLE_MS;
    g_policy_params.duty_percent       = RIVR_PARAM_DUTY_PERCENT;
    g_policy_params.role               = (uint8_t)RIVR_NODE_ROLE_CLIENT;
    /* Zero all metrics counters on (re)init. */
    s_policy_metrics.params_update_count       = 0u;
    s_policy_metrics.last_params_update_uptime_ms = 0u;
    s_policy_metrics.policy_rebuild_count      = 0u;
    s_policy_metrics.policy_reload_count       = 0u;
    s_policy_metrics.duty_blocked_count        = 0u;
    s_policy_metrics.origination_drop_count    = 0u;
    s_last_chat_orig_ms = 0u;
    s_last_data_orig_ms = 0u;
    RIVR_LOGI(TAG, "policy init: beacon=%lums chat=%lums data=%lums duty=%u%%",
              (unsigned long)g_policy_params.beacon_interval_ms,
              (unsigned long)g_policy_params.chat_throttle_ms,
              (unsigned long)g_policy_params.data_throttle_ms,
              (unsigned)g_policy_params.duty_percent);
}

void rivr_policy_set_param(uint8_t param_id, uint32_t value)
{
    switch ((rivr_param_id_t)param_id) {
        case RIVR_PARAM_ID_BEACON_INTERVAL:
            if (value < 1000u) {
                RIVR_LOGW(TAG, "beacon_interval_ms=%lu too small (min 1000), ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.beacon_interval_ms = value;
            RIVR_LOGI(TAG, "param update: beacon_interval_ms=%lu", (unsigned long)value);
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
        case 1u: /* PKT_CHAT */
            throttle_ms = g_policy_params.chat_throttle_ms;
            last_ms     = &s_last_chat_orig_ms;
            break;
        case 6u: /* PKT_DATA */
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
        (unsigned long)g_policy_params.beacon_interval_ms,
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
           "\"chat\":%lu,"
           "\"data\":%lu,"
           "\"duty\":%u,"
           "\"role\":\"%s\","
           "\"updates\":%lu,"
           "\"last_update_ms\":%lu,"
           "\"rebuilds\":%lu,"
           "\"reloads\":%lu,"
           "\"duty_blocked\":%lu,"
           "\"orig_drops\":%lu"
           "}\r\n",
           (unsigned long)g_policy_params.beacon_interval_ms,
           (unsigned long)g_policy_params.chat_throttle_ms,
           (unsigned long)g_policy_params.data_throttle_ms,
           (unsigned)g_policy_params.duty_percent,
           rivr_policy_role_name(g_policy_params.role),
           (unsigned long)s_policy_metrics.params_update_count,
           (unsigned long)s_policy_metrics.last_params_update_uptime_ms,
           (unsigned long)s_policy_metrics.policy_rebuild_count,
           (unsigned long)s_policy_metrics.policy_reload_count,
           (unsigned long)s_policy_metrics.duty_blocked_count,
           (unsigned long)s_policy_metrics.origination_drop_count);
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

    /* Invalid beacon interval (below minimum) → counter must NOT increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_BEACON_INTERVAL, 500u);
    assert(m->params_update_count == 1u);

    /* Valid chat throttle → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, 500u);
    assert(m->params_update_count == 2u);

    /* Invalid chat throttle (below 100 ms) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_CHAT_THROTTLE, 50u);
    assert(m->params_update_count == 2u);

    /* Valid data throttle → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_DATA_THROTTLE, 1000u);
    assert(m->params_update_count == 3u);

    /* Valid duty percent → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, 5u);
    assert(m->params_update_count == 4u);

    /* Invalid duty (out of [1..10]) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, 11u);
    assert(m->params_update_count == 4u);
    rivr_policy_set_param(RIVR_PARAM_ID_DUTY_PERCENT, 0u);
    assert(m->params_update_count == 4u);

    /* Unknown param_id → no increment. */
    rivr_policy_set_param(99u, 1000u);
    assert(m->params_update_count == 4u);

    /* ── Role parameter tests ── */

    /* Valid role (repeater) → increments. */
    rivr_policy_set_param(RIVR_PARAM_ID_ROLE, (uint32_t)RIVR_NODE_ROLE_REPEATER);
    assert(m->params_update_count == 5u);
    assert(g_policy_params.role == (uint8_t)RIVR_NODE_ROLE_REPEATER);

    /* Invalid role (out of [1..3]) → no increment. */
    rivr_policy_set_param(RIVR_PARAM_ID_ROLE, 99u);
    assert(m->params_update_count == 5u);
    rivr_policy_set_param(RIVR_PARAM_ID_ROLE, 0u);
    assert(m->params_update_count == 5u);

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

    RIVR_LOGI(TAG, "RIVR_POLICY_SELFTEST: all assertions passed (including role + origination tests)");
}

#endif /* RIVR_POLICY_SELFTEST */
