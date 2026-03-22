/**
 * @file  variants/heltec_t114/main_nrf52.cpp
 * @brief Arduino entry-point for the Rivr firmware on nRF52840 variants.
 *
 * ARCHITECTURE
 * ────────────
 *  setup()           — init Serial, derive node ID, start rivr_main_task
 *  loop()            — simply yields (FreeRTOS drives everything)
 *  rivr_main_task()  — mirrors app_main() from firmware_core/main.c:
 *      1. platform_init()       GPIO, SPI
 *      2. timebase_init()       1-ms FreeRTOS timer
 *      3. radio_init()          SX1262 register set-up + DIO1 ISR
 *      4. dutycycle_init()      EU868 duty-cycle context
 *      5. airtime_sched_init()  AirTime scheduler buckets
 *      6. load identity         compile-time defaults (NVS not available)
 *      7. rivr subsystems       policy → fabric → embed → bus
 *      8. boot banner + role
 *      9. radio_start_rx()      enter continuous RX
 *     10. main loop             rx → tick → tx → sleep 1 ms
 *
 * TX DRAIN (simplified vs main.c)
 * ────────────────────────────────
 *  Pop from rf_tx_queue, gate on jitter timestamp, check duty cycle,
 *  then radio_transmit().  Opportunistic relay suppression is included
 *  when RIVR_FEATURE_OPPORTUNISTIC_FWD is enabled.
 *
 * DEVICE ID
 * ─────────
 *  Derived from the nRF52840 FICR device ID registers (64-bit unique ID
 *  baked in at the factory).  Lower 32 bits XOR upper 32 bits produces a
 *  unique 32-bit node ID identical in style to the ESP32 MAC-based ID.
 */

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>

/* ── All C firmware headers ──────────────────────────────────────────────── */
extern "C" {
#include "firmware_core/rivr_config.h"
#include "firmware_core/platform_nrf52.h"
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
#include "firmware_core/ble/rivr_ble.h"  /* stubs when RIVR_FEATURE_BLE=0 */
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
#ifndef RIVR_PLATFORM_NAME
#  define RIVR_PLATFORM_NAME "nRF52840"
#endif

/* ── nRF52840 FICR device ID registers (factory-programmed unique ID) ────── */
#define NRF52_FICR_DEVICEID0  (*(volatile uint32_t *)0x10000060UL)
#define NRF52_FICR_DEVICEID1  (*(volatile uint32_t *)0x10000064UL)

/* ═══════════════════════════════════════════════════════════════════════════
 * TX drain helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static void tx_drain_loop(void)
{
    uint32_t now_ms = tb_millis();

    for (uint32_t i = 0u; i < TX_DRAIN_LIMIT; i++) {
        rf_tx_request_t req;
        if (!rb_pop(&rf_tx_queue, &req)) break;   /* queue empty */

        /* Jitter gate: hold the frame until its due_ms timestamp */
        if (req.due_ms != 0u && req.due_ms > now_ms) {
            if (!rb_try_push(&rf_tx_queue, &req)) {
                RIVR_LOGW(TAG, "TX jitter re-push failed – frame dropped");
            }
            continue;
        }

        /* Decode to detect relay frames */
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

        /* Airtime scheduler gate */
        if (!airtime_sched_check_consume(req.data, req.len, req.toa_us, now_ms)) {
            continue;
        }

        /* Duty-cycle gate */
        if (!dutycycle_check(&g_dc, now_ms, req.toa_us)) {
            RIVR_LOGW(TAG, "TX duty-cycle gate: toa=%u us dropped", req.toa_us);
            rivr_fabric_on_tx_blocked_dc(now_ms, req.toa_us);
            continue;
        }

        /* Transmit */
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Main RIVR task (FreeRTOS)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rivr_main_task(void *pvParameters)
{
    (void)pvParameters;

    /* ── 0. Crash recovery report (no-op on nRF52 first boot) ────────────── */
    rivr_panic_check_prev();

    RIVR_LOGI(TAG, "=== RIVR Embedded Node booting (%s) ===", RIVR_PLATFORM_NAME);
    RIVR_LOGI(TAG, "Platform: %s / nRF52840 (Arduino BSP)", RIVR_PLATFORM_NAME);

    /* ── 1. Hardware peripherals ─────────────────────────────────────────── */
    platform_init();
    timebase_init();
    radio_init();
    dutycycle_init(&g_dc);
    airtime_sched_init();

    /* ── 2. Node identity ────────────────────────────────────────────────── */
    /* Read 64-bit factory ID from FICR and fold to 32 bits */
    uint32_t ficr_lo = NRF52_FICR_DEVICEID0;
    uint32_t ficr_hi = NRF52_FICR_DEVICEID1;
    g_my_node_id = ficr_lo ^ ficr_hi;
    RIVR_LOGI(TAG, "Node ID: 0x%08lX (from nRF52840 FICR)", (unsigned long)g_my_node_id);

    g_net_id = (uint16_t)RIVR_NET_ID;
    strncpy(g_callsign, RIVR_CALLSIGN, sizeof(g_callsign) - 1u);
    g_callsign[sizeof(g_callsign) - 1u] = '\0';
    /* NVS not available on nRF52 — compile-time defaults always used */
    rivr_nvs_load_identity();     /* no-op: nvs_open returns ESP_FAIL */

    /* ── 3. BLE transport (nRF52 Bluefruit backend) ──────────────────────── */
    /* Must be called after g_my_node_id is set so the device name includes   */
    /* the node ID.  No-op when RIVR_FEATURE_BLE=0 (stub in rivr_ble.h).     */
    rivr_ble_init();

    /* ── 4. RIVR engine ──────────────────────────────────────────────────── */
    rivr_policy_init();
    rivr_fabric_init();
    rivr_embed_init();
    rivr_bus_init();

    /* ── 4. Boot banner ──────────────────────────────────────────────────── */
    build_info_print_banner();

    /* ── 5. Role-specific init ───────────────────────────────────────────── */
#if RIVR_ROLE_CLIENT
    RIVR_LOGI(TAG, "role: CLIENT | relay_budget=%u | CLI: Arduino Serial",
              (unsigned)FWDBUDGET_MAX_FWD_ROLE);
    rivr_cli_init();   /* sets up Arduino Serial + prints boot banner */
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

    /* ── 8. Main loop ────────────────────────────────────────────────────── */
    uint32_t last_stats_ms  = 0u;
    uint32_t loop_count     = 0u;

    for (;;) {
#if RIVR_ROLE_CLIENT
        /* CLI must run before rivr_tick() to own UART bytes first */
        rivr_cli_poll();
#endif

        /* Service DIO1 events (RxDone / TxDone) */
        radio_service_rx();

        /* Check for prolonged RX silence */
        radio_check_timeouts();

        /* RIVR processing tick */
        rivr_tick();

        /* BLE timeout state machine — no-op when RIVR_FEATURE_BLE=0 */
        rivr_ble_tick(tb_millis());

        /* TX drain with duty-cycle gate */
        tx_drain_loop();

        /* Hot-reload check */
        if (g_program_reload_pending) {
            rivr_embed_reload();
        }

        /* Periodic diagnostics */
        uint32_t now = tb_millis();
        rivr_fabric_tick(now);

        if (now - last_stats_ms >= STATS_INTERVAL_MS) {
            last_stats_ms = now;
            RIVR_LOGI(TAG, "-- stats (loop#%lu, uptime=%lu ms) --",
                     (unsigned long)loop_count, (unsigned long)now);
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
        loop_count++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Arduino entry points
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup(void)
{
    /* Open USB CDC serial for debug output */
    Serial.begin(115200);
    /* Wait up to 2 s for serial monitor — optional, skip on production */
#if RIVR_SERIAL_WAIT_MS
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < (uint32_t)RIVR_SERIAL_WAIT_MS) {}
#endif

    /* Launch the main RIVR task.  Stack size of 8 KB is generous for nRF52
     * (256 KB SRAM total); adjust if the stack checker reports overflow. */
    xTaskCreate(rivr_main_task,
                "rivr",
                8192u / sizeof(StackType_t),  /* stack words */
                NULL,
                2,           /* priority — above the idle task (0) */
                NULL);
}

void loop(void)
{
    /* All work is done in rivr_main_task(); loop() just yields. */
    vTaskDelay(portMAX_DELAY);
}
