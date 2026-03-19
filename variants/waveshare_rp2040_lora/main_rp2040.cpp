/**
 * @file  variants/waveshare_rp2040_lora/main_rp2040.cpp
 * @brief Arduino entry-point for the Rivr firmware on the Waveshare RP2040 LoRa
 *        (RP2040 + SX1262, arduino-pico framework, no FreeRTOS).
 *
 * ARCHITECTURE
 * ────────────
 *  setup()  — init Serial, derive node ID, run full firmware init
 *  loop()   — main event loop (replaces FreeRTOS task)
 *      1. platform_init()       GPIO, SPI1
 *      2. timebase_init()       1-ms pico hardware repeating timer
 *      3. radio_init()          SX1262 register set-up + DIO1 ISR
 *      4. dutycycle_init()      EU868 duty-cycle context
 *      5. airtime_sched_init()  AirTime scheduler buckets
 *      6. load identity         compile-time defaults (no NVS on RP2040)
 *      7. rivr subsystems       policy → fabric → embed → bus
 *      8. boot banner + role
 *      9. radio_start_rx()      enter continuous RX
 *     10. main loop in loop()   rx → tick → tx → sleep
 *
 * DEVICE ID
 * ─────────
 *  Derived from the RP2040's unique 64-bit flash ID (pico_get_unique_board_id).
 *  The 8 bytes are XOR'd into two 32-bit halves, then those are XOR'd to give
 *  a unique 32-bit node ID.
 */

#include <Arduino.h>
#include <pico/unique_id.h>   /* pico_get_unique_board_id */

/* ── All C firmware headers ──────────────────────────────────────────────── */
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
#include "rivr_layer/rivr_embed.h"
#include "rivr_layer/rivr_sinks.h"
#include "rivr_layer/rivr_cli.h"
#include "firmware_core/rivr_bus/rivr_bus.h"
} /* extern "C" */

#define TAG              "MAIN"
#define TX_DRAIN_LIMIT   2u
#define STATS_INTERVAL_MS 30000u

/* ── Compile-time identity defaults ──────────────────────────────────────── */
#ifndef RIVR_CALLSIGN
#  define RIVR_CALLSIGN "RIVR"
#endif
#ifndef RIVR_NET_ID
#  define RIVR_NET_ID   0u
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * TX drain helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static void tx_drain_loop(void)
{
    uint32_t now_ms = tb_millis();

    for (uint32_t i = 0u; i < TX_DRAIN_LIMIT; i++) {
        rf_tx_request_t req;
        if (!rb_pop(&rf_tx_queue, &req)) break;

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
                opfwd_suppress_check(&g_opfwd_suppress,
                                     req_hdr.src_id, req_hdr.pkt_id,
                                     now_ms)) {
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
            if (req_is_relay) { g_rivr_metrics.relay_forwarded_total++; }
        } else {
            rivr_fabric_on_tx_fail(tb_millis(), req.toa_us);
            RIVR_LOGE(TAG, "TX failed");
        }
    }
}

/* ── Tracking state for loop() ────────────────────────────────────────────── */
static uint32_t s_last_stats_ms = 0u;
static uint32_t s_loop_count    = 0u;

/* ═══════════════════════════════════════════════════════════════════════════
 * Arduino entry points
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup(void)
{
    /* Native USB CDC: baud rate is ignored; wait briefly for monitor */
    Serial.begin(115200);
#if RIVR_SERIAL_WAIT_MS
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < (uint32_t)RIVR_SERIAL_WAIT_MS) {}
#endif

    /* ── 0. Crash recovery report (no-op on RP2040 first boot) ──────────── */
    rivr_panic_check_prev();

    RIVR_LOGI(TAG, "=== RIVR Embedded Node booting (RP2040) ===");
    RIVR_LOGI(TAG, "Platform: Waveshare RP2040 LoRa (arduino-pico)");

    /* ── 1. Hardware peripherals ─────────────────────────────────────────── */
    platform_init();
    timebase_init();
    radio_init();
    dutycycle_init(&g_dc);
    airtime_sched_init();

    /* ── 2. Node identity ────────────────────────────────────────────────── */
    /* Read 64-bit unique flash ID and fold to 32 bits */
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    uint32_t id_lo = ((uint32_t)uid.id[0]       |
                      ((uint32_t)uid.id[1] << 8)  |
                      ((uint32_t)uid.id[2] << 16) |
                      ((uint32_t)uid.id[3] << 24));
    uint32_t id_hi = ((uint32_t)uid.id[4]       |
                      ((uint32_t)uid.id[5] << 8)  |
                      ((uint32_t)uid.id[6] << 16) |
                      ((uint32_t)uid.id[7] << 24));
    g_my_node_id = id_lo ^ id_hi;
    RIVR_LOGI(TAG, "Node ID: 0x%08lX (from RP2040 flash UID)", (unsigned long)g_my_node_id);

    g_net_id = (uint16_t)RIVR_NET_ID;
    strncpy(g_callsign, RIVR_CALLSIGN, sizeof(g_callsign) - 1u);
    g_callsign[sizeof(g_callsign) - 1u] = '\0';
    rivr_nvs_load_identity();  /* no-op: nvs_open returns ESP_FAIL */

    /* ── 3. RIVR engine ──────────────────────────────────────────────────── */
    rivr_policy_init();
    rivr_fabric_init();
    rivr_embed_init();
    rivr_bus_init();

    /* ── 4. Boot banner ──────────────────────────────────────────────────── */
    build_info_print_banner();

    /* ── 5. Role-specific init ───────────────────────────────────────────── */
#if RIVR_ROLE_CLIENT
    RIVR_LOGI(TAG, "role: CLIENT | relay_budget=%u | CLI: USB Serial",
              (unsigned)FWDBUDGET_MAX_FWD_ROLE);
    rivr_cli_init();
#elif RIVR_ROLE_REPEATER
    RIVR_LOGI(TAG, "role: REPEATER | relay_budget=%u | fabric=%s",
              (unsigned)FWDBUDGET_MAX_FWD_ROLE,
              RIVR_FABRIC_REPEATER ? "on" : "off");
#endif

    /* ── 6. Boot support pack ────────────────────────────────────────────── */
    {
        char sp_buf[768];
        build_info_write_supportpack(sp_buf, sizeof(sp_buf),
            0u, 0u, 0u,
            (uint64_t)36000000u, 0u, 0u,
            0u, 0u, 0u);
        printf("@SUPPORTPACK %s\r\n", sp_buf);
        fflush(stdout);
    }

    /* ── 7. Start radio receive ──────────────────────────────────────────── */
    radio_start_rx();
    RIVR_LOGI(TAG, "entering main loop");
}

void loop(void)
{
#if RIVR_ROLE_CLIENT
    rivr_cli_poll();
#endif

    radio_service_rx();
    radio_check_timeouts();
    rivr_tick();
    tx_drain_loop();

    if (g_program_reload_pending) {
        rivr_embed_reload();
    }

    uint32_t now = tb_millis();
    rivr_fabric_tick(now);

    if (now - s_last_stats_ms >= STATS_INTERVAL_MS) {
        s_last_stats_ms = now;
        RIVR_LOGI(TAG, "-- stats (loop#%lu, uptime=%lu ms) --",
                 (unsigned long)s_loop_count, (unsigned long)now);
        RIVR_LOGI(TAG, "  rx_drops: %u  tx_drops: %u",
                 rb_drops(&rf_rx_ringbuf), rb_drops(&rf_tx_queue));
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

    platform_led_toggle();
    s_loop_count++;

    /* Small cooperative yield — avoids busy-spinning the single M0+ core */
    delay(1);
}
