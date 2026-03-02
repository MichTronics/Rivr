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

#define TAG "RIVR_POLICY"

/* ── Global policy parameter state ──────────────────────────────────────── */

rivr_policy_params_t g_policy_params;

/* ── Policy metrics (private — exposed via rivr_policy_metrics_get()) ────── */

static rivr_policy_metrics_t s_policy_metrics;

/* ── API ─────────────────────────────────────────────────────────────────── */

void rivr_policy_init(void)
{
    g_policy_params.beacon_interval_ms = RIVR_PARAM_BEACON_INTERVAL_MS;
    g_policy_params.chat_throttle_ms   = RIVR_PARAM_CHAT_THROTTLE_MS;
    g_policy_params.data_throttle_ms   = RIVR_PARAM_DATA_THROTTLE_MS;
    g_policy_params.duty_percent       = RIVR_PARAM_DUTY_PERCENT;
    /* Zero all metrics counters on (re)init. */
    s_policy_metrics.params_update_count       = 0u;
    s_policy_metrics.last_params_update_uptime_ms = 0u;
    s_policy_metrics.policy_rebuild_count      = 0u;
    s_policy_metrics.policy_reload_count       = 0u;
    s_policy_metrics.duty_blocked_count        = 0u;
    s_policy_metrics.origination_drop_count    = 0u;
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

        default:
            RIVR_LOGW(TAG, "unknown param_id=%u, ignored", (unsigned)param_id);
            break;
    }
}

void rivr_policy_build_program(char *buf, size_t bufsz)
{
    /*
     * Generated program shape (RIVR_MESH_PROGRAM with runtime params):
     *
     *   source rf_rx @lmp = rf;
     *   source beacon_tick = timer(<beacon_interval_ms>);
     *
     *   let chat = rf_rx
     *     |> filter.pkt_type(1)
     *     |> budget.toa_us(280000, 0.<duty_percent:02d>, 280000)
     *     |> throttle.ms(<chat_throttle_ms>);
     *
     *   let data = rf_rx
     *     |> filter.pkt_type(6)
     *     |> throttle.ms(<data_throttle_ms>);
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
        (unsigned long)g_policy_params.chat_throttle_ms,
        (unsigned long)g_policy_params.data_throttle_ms);

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

    RIVR_LOGI(TAG, "RIVR_POLICY_SELFTEST: all %u assertions passed",
              (unsigned)m->params_update_count + 5u);
}

#endif /* RIVR_POLICY_SELFTEST */
