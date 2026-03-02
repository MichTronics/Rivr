/*
 * rivr_policy.c — Runtime-adjustable Rivr policy parameter implementation.
 *
 * See rivr_policy.h for full documentation.
 */

#include "rivr_policy.h"
#include "rivr_programs/default_program.h"   /* RIVR_PARAM_* compiled-in defaults */
#include "rivr_log.h"
#include <stdio.h>
#include <string.h>

#define TAG "RIVR_POLICY"

/* ── Global policy parameter state ──────────────────────────────────────── */

rivr_policy_params_t g_policy_params;

/* ── API ─────────────────────────────────────────────────────────────────── */

void rivr_policy_init(void)
{
    g_policy_params.beacon_interval_ms = RIVR_PARAM_BEACON_INTERVAL_MS;
    g_policy_params.chat_throttle_ms   = RIVR_PARAM_CHAT_THROTTLE_MS;
    g_policy_params.data_throttle_ms   = RIVR_PARAM_DATA_THROTTLE_MS;
    g_policy_params.duty_percent       = RIVR_PARAM_DUTY_PERCENT;
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
            break;

        case RIVR_PARAM_ID_CHAT_THROTTLE:
            if (value < 100u) {
                RIVR_LOGW(TAG, "chat_throttle_ms=%lu too small (min 100), ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.chat_throttle_ms = value;
            RIVR_LOGI(TAG, "param update: chat_throttle_ms=%lu", (unsigned long)value);
            break;

        case RIVR_PARAM_ID_DATA_THROTTLE:
            if (value < 100u) {
                RIVR_LOGW(TAG, "data_throttle_ms=%lu too small (min 100), ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.data_throttle_ms = value;
            RIVR_LOGI(TAG, "param update: data_throttle_ms=%lu", (unsigned long)value);
            break;

        case RIVR_PARAM_ID_DUTY_PERCENT:
            if (value < 1u || value > 10u) {
                RIVR_LOGW(TAG, "duty_percent=%lu out of range [1..10], ignored",
                          (unsigned long)value);
                return;
            }
            g_policy_params.duty_percent = (uint8_t)value;
            RIVR_LOGI(TAG, "param update: duty_percent=%u", (unsigned)value);
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
     */
    snprintf(buf, bufsz,
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

    /* Guarantee NUL termination even if snprintf truncated. */
    buf[bufsz - 1u] = '\0';
}
