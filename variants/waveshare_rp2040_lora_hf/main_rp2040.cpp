/**
 * @file  variants/waveshare_rp2040_lora_hf/main_rp2040.cpp
 * @brief Arduino entry-point for the Waveshare RP2040-LoRa-HF.
 */

#include <Arduino.h>
#include <pico/unique_id.h>

extern "C" {
#include "firmware_core/rivr_config.h"
#include "firmware_core/platform_rp2040.h"
#include "firmware_core/timebase.h"
#include "firmware_core/radio_sx1262.h"
#include "firmware_core/dutycycle.h"
#include "firmware_core/airtime_sched.h"
#include "firmware_core/protocol.h"
#include "firmware_core/routing.h"
#include "firmware_core/route_cache.h"
#include "firmware_core/pending_queue.h"
#include "firmware_core/rivr_fabric.h"
#include "firmware_core/rivr_metrics.h"
#include "firmware_core/rivr_log.h"
#include "firmware_core/rivr_ota.h"
#include "firmware_core/build_info.h"
#include "firmware_core/rivr_panic.h"
#include "firmware_core/rivr_policy.h"
#include "firmware_core/opfwd_suppress.h"
#include "firmware_core/send_queue.h"
#include "rivr_layer/rivr_embed.h"
#include "rivr_layer/rivr_sinks.h"
#include "rivr_layer/rivr_cli.h"
#include "firmware_core/rivr_bus/rivr_bus.h"
#include "firmware_core/ble/rivr_ble.h"
#include "firmware_core/ble/rivr_ble_companion.h"
}

#define TAG "MAIN"
#define TX_DRAIN_LIMIT 2u
#define STATS_INTERVAL_MS 30000u
#define USB_GRACE_MS 1500u

/* ── Global originated-message outbox (defined here; extern in rivr_embed.h) */
send_queue_t g_send_queue;

#ifndef RIVR_RP2040_USB_DEBUG
#  define RIVR_RP2040_USB_DEBUG 0
#endif

#ifndef RIVR_CALLSIGN
#  define RIVR_CALLSIGN "RIVR"
#endif
#ifndef RIVR_NET_ID
#  define RIVR_NET_ID 0u
#endif
#ifndef RIVR_PLATFORM_NAME
#  define RIVR_PLATFORM_NAME "RP2040"
#endif

static bool s_cli_session_announced = false;

static uint32_t fold_unique_id(void)
{
    pico_unique_board_id_t src;
    pico_get_unique_board_id(&src);

    uint32_t id = 0u;
    for (size_t i = 0; i < sizeof(src.id); i++) {
        id = (id << 5) ^ (id >> 2) ^ src.id[i];
    }
    return id;
}

extern "C" bool rivr_arduino_uart_driver_ready(void)
{
    return true;
}

extern "C" int rivr_arduino_uart_read(void *buf, uint32_t length)
{
    if (!buf || length == 0u) {
        return 0;
    }

    uint8_t *dst = static_cast<uint8_t *>(buf);
    uint32_t count = 0u;
    while (count < length && Serial.available() > 0) {
        int ch = Serial.read();
        if (ch < 0) {
            break;
        }
        dst[count++] = (uint8_t)ch;
    }
    return (int)count;
}

extern "C" int rivr_arduino_uart_write(const void *src, size_t size)
{
    if (!src || size == 0u) {
        return 0;
    }
    return (int)Serial.write((const uint8_t *)src, size);
}

static bool serial_session_active(void)
{
    return (bool)Serial || Serial.dtr() || Serial.available() > 0;
}

static void cli_session_tick(void)
{
#if RIVR_ROLE_CLIENT
    bool active = serial_session_active();
    if (active && !s_cli_session_announced) {
        rivr_cli_init();
        s_cli_session_announced = true;
    } else if (!active) {
        s_cli_session_announced = false;
    }
#endif
}

static void tx_drain_loop(void)
{
    uint32_t now_ms = tb_millis();

    for (uint32_t i = 0u; i < TX_DRAIN_LIMIT; i++) {
        rf_tx_request_t req;
        if (!rb_pop(&rf_tx_queue, &req)) {
            break;
        }

        if (req.due_ms != 0u && req.due_ms > now_ms) {
            if (!rb_try_push(&rf_tx_queue, &req)) {
                RIVR_LOGW(TAG, "TX jitter re-push failed – frame dropped");
            }
            continue;
        }

        bool req_is_relay = false;
        rivr_pkt_hdr_t req_hdr;
        const uint8_t *req_pl = NULL;
        if (protocol_decode(req.data, req.len, &req_hdr, &req_pl)) {
            req_is_relay = (req_hdr.flags & PKT_FLAG_RELAY) != 0u;
        }

#if RIVR_FEATURE_OPPORTUNISTIC_FWD
        if (req_is_relay &&
            opfwd_suppress_check(&g_opfwd_suppress, req_hdr.src_id, req_hdr.pkt_id, now_ms)) {
            g_rivr_metrics.flood_fwd_cancelled_opport_total++;
            continue;
        }
#endif

        if (!airtime_sched_check_consume(req.data, req.len, req.toa_us, now_ms)) {
            continue;
        }

        if (!dutycycle_check(&g_dc, now_ms, req.toa_us)) {
            RIVR_LOGW(TAG, "TX duty-cycle gate: toa=%u us dropped", req.toa_us);
            rivr_fabric_on_tx_blocked_dc(now_ms, req.toa_us);
            continue;
        }

        rivr_fabric_on_tx_enqueued(now_ms, req.toa_us);
        platform_led_on();
        bool ok = radio_transmit(&req);
        platform_led_off();

        if (ok) {
            dutycycle_record(&g_dc, tb_millis(), req.toa_us);
            rivr_fabric_on_tx_ok(tb_millis(), req.toa_us);
            if (req_is_relay) {
                g_rivr_metrics.relay_forwarded_total++;
            }
        } else {
            rivr_fabric_on_tx_fail(tb_millis(), req.toa_us);
            RIVR_LOGE(TAG, "TX failed");
        }
    }
}

static void rivr_boot(void)
{
    rivr_panic_check_prev();

    RIVR_LOGI(TAG, "=== RIVR Embedded Node booting (%s) ===", RIVR_PLATFORM_NAME);
    RIVR_LOGI(TAG, "Platform: %s / RP2040 (Arduino core)", RIVR_PLATFORM_NAME);

    timebase_init();
    timebase_tick_hook();

#if RIVR_RP2040_USB_DEBUG
    radio_init_buffers_only();
    RIVR_LOGW(TAG, "USB debug build: SX1262 bring-up skipped");
#else
    platform_init();
    radio_init();
    dutycycle_init(&g_dc);
    airtime_sched_init();
#endif

    g_my_node_id = fold_unique_id();
    RIVR_LOGI(TAG, "Node ID: 0x%08lX (from RP2040 unique board ID)",
              (unsigned long)g_my_node_id);

    g_net_id = (uint16_t)RIVR_NET_ID;
    strncpy(g_callsign, RIVR_CALLSIGN, sizeof(g_callsign) - 1u);
    g_callsign[sizeof(g_callsign) - 1u] = '\0';
    rivr_nvs_load_identity();

    rivr_ble_init();
    rivr_policy_init();
    rivr_fabric_init();
    rivr_embed_init();
    rivr_bus_init();
    send_queue_init(&g_send_queue);

    build_info_print_banner();

#if RIVR_ROLE_CLIENT
    RIVR_LOGI(TAG, "role: CLIENT | relay_budget=%u | CLI: Arduino Serial",
              (unsigned)FWDBUDGET_MAX_FWD_ROLE);
    rivr_cli_init();
    s_cli_session_announced = serial_session_active();
#elif RIVR_ROLE_REPEATER
    RIVR_LOGI(TAG, "role: REPEATER | relay_budget=%u | fabric=%s",
              (unsigned)FWDBUDGET_MAX_FWD_ROLE,
              RIVR_FABRIC_REPEATER ? "on" : "off");
#endif

    {
        char sp_buf[768];
        build_info_write_supportpack(sp_buf, sizeof(sp_buf),
                                     0u, 0u, 0u,
                                     (uint64_t)36000000u, 0u, 0u,
                                     0u, 0u, 0u);
        printf("@SUPPORTPACK %s\r\n", sp_buf);
        fflush(stdout);
    }

#if !RIVR_RP2040_USB_DEBUG
    radio_start_rx();
#endif
    RIVR_LOGI(TAG, "entering main loop");
}

void setup(void)
{
    Serial.begin(115200);
    Serial.ignoreFlowControl(true);
    delay(USB_GRACE_MS);

    rivr_boot();
}

void loop(void)
{
    static uint32_t last_stats_ms = 0u;
    static uint32_t last_met_print = 0u;
    static uint32_t loop_count = 0u;
    uint32_t loop_body_start = tb_millis();

    timebase_tick_hook();
    cli_session_tick();

#if RIVR_ROLE_CLIENT
    rivr_cli_poll();
#endif

#if !RIVR_RP2040_USB_DEBUG
    radio_service_rx();
    radio_check_timeouts();
#endif
    rivr_tick();
    rivr_ble_tick(tb_millis());
    rivr_ble_companion_tick();
    send_queue_tick(&g_send_queue, &rf_tx_queue, tb_millis());
#if !RIVR_RP2040_USB_DEBUG
    tx_drain_loop();
#endif

    if (g_program_reload_pending) {
        rivr_embed_reload();
    }

    {
        uint32_t now = tb_millis();
        rivr_fabric_tick(now);

        if (now - last_stats_ms >= STATS_INTERVAL_MS) {
            last_stats_ms = now;
            RIVR_LOGI(TAG, "-- stats (loop#%lu, uptime=%lu ms) --",
                      (unsigned long)loop_count,
                      (unsigned long)now);
            RIVR_LOGI(TAG, "  rx_drops: %u  tx_drops: %u",
                      rb_drops(&rf_rx_ringbuf),
                      rb_drops(&rf_tx_queue));
            rivr_embed_print_stats();

            char sp_buf[768];
            build_info_write_supportpack(
                sp_buf, sizeof(sp_buf),
                routing_neighbor_count(&g_neighbor_table, now),
                route_cache_count(&g_route_cache, now),
                pending_queue_count(&g_pending_queue),
                dutycycle_remaining_us(&g_dc),
                g_dc.used_us,
                g_dc.blocked_count,
                now,
                g_rx_frame_count,
                g_tx_frame_count);
            printf("@SUPPORTPACK %s\r\n", sp_buf);
            fflush(stdout);
        }

#if RIVR_FEATURE_METRICS
        if (now - last_met_print >= 5000u) {
            last_met_print = now;
            if (rivr_log_get_mode() != RIVR_LOG_SILENT) {
                const neighbor_link_summary_t lnk =
                    neighbor_table_link_summary(&g_ntable, now);
                const rivr_live_stats_t ls = {
                    .node_id       = g_my_node_id,
                    .dc_pct        = (uint8_t)(DC_BUDGET_US > 0u
                                     ? ((DC_BUDGET_US - dutycycle_remaining_us(&g_dc)) * 100ULL
                                        / DC_BUDGET_US)
                                     : 0u),
                    .q_depth       = (uint8_t)rb_available(&rf_tx_queue),
                    .tx_total      = g_tx_frame_count,
                    .rx_total      = g_rx_frame_count,
                    .route_cache   = route_cache_count(&g_route_cache, now),
                    .lnk_cnt       = lnk.count,
                    .lnk_best      = lnk.best_score,
                    .lnk_best_rssi = lnk.best_rssi,
                    .lnk_avg_loss  = lnk.avg_loss,
                    .relay_density = lnk.count,
                };
                rivr_metrics_print(&ls);
                rivr_metrics_ble_push(&ls, g_my_node_id, g_net_id,
                                      (uint16_t)++g_ctrl_seq);
            }
        }
#endif

        if (loop_count == 6000u) {
            rivr_panic_clear_reset_count();
        }
    }

    {
        uint32_t iter_ms = tb_millis() - loop_body_start;
        if (iter_ms > g_rivr_metrics.loop_jitter_ms) {
            g_rivr_metrics.loop_jitter_ms = iter_ms;
        }
    }

    loop_count++;
    delay(10);
    timebase_tick_hook();
}
