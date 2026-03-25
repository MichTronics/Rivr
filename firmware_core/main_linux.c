/**
 * @file  main_linux.c
 * @brief Raspberry Pi Linux firmware entry point — RIVR-driven LoRa node.
 *
 * MAIN LOOP STRUCTURE
 * ═══════════════════
 *
 *  main()
 *   │
 *   ├─ platform_init()        SPI (spidev), GPIO (libgpiod), DIO1 thread
 *   ├─ timebase_init()        start 1-ms monotonic pthread timer
 *   ├─ radio_init()           configure SX1262, start DIO1 thread
 *   ├─ dutycycle_init()       zero duty-cycle sliding-window context
 *   ├─ airtime_sched_init()   zero airtime token-bucket context
 *   ├─ rivr_policy_init()     load compiled-in policy defaults
 *   ├─ rivr_fabric_init()     init fabric relay state machine
 *   ├─ rivr_embed_init()      parse RIVR program, register sinks
 *   ├─ rivr_bus_init()        zero dup-cache, log transport init
 *   └─ radio_start_rx()       start continuous receive
 *        │
 *        └─ loop forever:
 *              radio_service_rx()    ← drain DIO1 events via SPI
 *              radio_check_timeouts()← silence watchdog
 *              rivr_tick()           ← drain RX, run engine, emit to sinks
 *              tx_drain_loop()       ← send queued TX frames (DC-gated)
 *              nanosleep(1 ms)       ← yield CPU
 *
 * NODE IDENTITY
 * ─────────────
 * The node ID is derived from the first available Ethernet or WLAN MAC
 * address read from /sys/class/net/<iface>/address.  Fallback: 0xDEADBEEF.
 *
 * CLI / SERIAL
 * ─────────────
 * stdin is set to O_NONBLOCK in platform_init().  When RIVR_ROLE_CLIENT=1,
 * rivr_cli_poll() drains typed commands from stdin each loop tick.
 *
 * No BLE, no display, no USB SLIP bridge, no task watchdog.
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>

/* Linux compat stubs must come before any rivr headers that include ESP-IDF. */
#include "firmware_core/linux/include/esp_log.h"
#include "firmware_core/linux/include/esp_err.h"

/* Rivr central configuration — sets safe defaults. */
#include "firmware_core/rivr_config.h"

#include "firmware_core/platform_linux.h"
#include "firmware_core/timebase.h"
#include "firmware_core/radio_sx1262.h"
#include "firmware_core/dutycycle.h"
#include "firmware_core/protocol.h"
#include "firmware_core/routing.h"
#include "firmware_core/route_cache.h"
#include "firmware_core/pending_queue.h"
#include "firmware_core/rivr_fabric.h"
#include "firmware_core/rivr_metrics.h"
#include "firmware_core/rivr_log.h"
#include "firmware_core/build_info.h"
#include "firmware_core/airtime_sched.h"
#include "firmware_core/rivr_panic.h"
#include "firmware_core/rivr_policy.h"
#include "firmware_core/opfwd_suppress.h"
#include "rivr_layer/rivr_embed.h"
#include "rivr_layer/rivr_sinks.h"
#include "rivr_layer/rivr_cli.h"
#include "firmware_core/rivr_bus/rivr_bus.h"

#define TAG              "MAIN"
#define TX_DRAIN_LIMIT   2u
#define STATS_INTERVAL_MS 30000u

/** Compile-time node callsign — override with -DRIVR_CALLSIGN=\"N0CALL\" */
#ifndef RIVR_CALLSIGN
#  define RIVR_CALLSIGN "RIVR"
#endif
/** Compile-time network discriminator */
#ifndef RIVR_NET_ID
#  define RIVR_NET_ID 0u
#endif

/* ── Graceful shutdown on SIGINT/SIGTERM ─────────────────────────────────── */
static volatile int s_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    s_running = 0;
}

/* ── Derive node ID from MAC address ─────────────────────────────────────── */

/**
 * Try to read a MAC address from /sys/class/net/<iface>/address and fold
 * the last 4 bytes into a 32-bit node ID.  Returns 0 on failure.
 */
static uint32_t read_mac_node_id(const char *iface)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);

    FILE *f = fopen(path, "r");
    if (!f) return 0u;

    unsigned int b[6] = {0};
    int n = fscanf(f, "%x:%x:%x:%x:%x:%x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    fclose(f);

    if (n != 6) return 0u;

    /* Use bytes [2..5] — same convention as the ESP32 WiFi MAC approach. */
    return ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16)
         | ((uint32_t)b[4] <<  8) |  (uint32_t)b[5];
}

static void derive_node_id(void)
{
    /* Try common interface names in order of preference. */
    const char *ifaces[] = { "eth0", "eth1", "wlan0", "wlan1", NULL };
    for (int i = 0; ifaces[i]; i++) {
        uint32_t id = read_mac_node_id(ifaces[i]);
        if (id != 0u) {
            g_my_node_id = id;
            RIVR_LOGI(TAG, "Node ID: 0x%08lX (from %s MAC)",
                     (unsigned long)g_my_node_id, ifaces[i]);
            return;
        }
    }
    /* Fallback — still operational, just not unique. */
    g_my_node_id = 0xDEADBEEFul;
    RIVR_LOGW(TAG, "No MAC found — using fallback node ID 0x%08lX",
             (unsigned long)g_my_node_id);
}

/* ── TX drain loop ───────────────────────────────────────────────────────── */

static void tx_drain_loop(void)
{
    uint32_t now_ms = tb_millis();

    for (uint32_t i = 0; i < TX_DRAIN_LIMIT; i++) {
        rf_tx_request_t req;
        if (!rb_pop(&rf_tx_queue, &req)) break;

        /* Jitter gate */
        if (req.due_ms != 0u && req.due_ms > now_ms) {
            if (!rb_try_push(&rf_tx_queue, &req)) {
                ESP_LOGW(TAG, "TX re-push failed for jittered frame – dropped");
            }
            continue;
        }

        /* Decode to identify relay frames */
        bool req_is_relay = false;
        rivr_pkt_hdr_t req_hdr;
        const uint8_t *req_pl = NULL;
        if (protocol_decode(req.data, req.len, &req_hdr, &req_pl)) {
            req_is_relay = (req_hdr.flags & PKT_FLAG_RELAY) != 0u;
        }

#if RIVR_FEATURE_OPPORTUNISTIC_FWD
        if (req_is_relay
                && opfwd_suppress_check(&g_opfwd_suppress,
                                        req_hdr.src_id, req_hdr.pkt_id,
                                        now_ms)) {
            g_rivr_metrics.flood_fwd_cancelled_opport_total++;
            continue;
        }
#endif

        /* Airtime + duty-cycle gate */
        if (!airtime_sched_check_consume(req.data, req.len, req.toa_us, now_ms)) {
            continue;
        }
        if (!dutycycle_check(&g_dc, now_ms, req.toa_us)) {
            ESP_LOGW(TAG, "TX frame dropped by duty-cycle gate (toa=%u us)", req.toa_us);
            rivr_fabric_on_tx_blocked_dc(now_ms, req.toa_us);
            continue;
        }

        /* Transmit */
        rivr_fabric_on_tx_enqueued(now_ms, req.toa_us);
        platform_led_on();
        bool tx_ok = radio_transmit(&req);
        platform_led_off();

        if (tx_ok) {
            dutycycle_record(&g_dc, tb_millis(), req.toa_us);
            rivr_fabric_on_tx_ok(tb_millis(), req.toa_us);
            if (req_is_relay) { g_rivr_metrics.relay_forwarded_total++; }
        } else {
            rivr_fabric_on_tx_fail(tb_millis(), req.toa_us);
            ESP_LOGE(TAG, "TX failed");
        }
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    RIVR_LOGI(TAG, "═══ RIVR Linux Node booting ═══");

    /* Hardware + timing */
    platform_init();
    timebase_init();
    radio_init();
    dutycycle_init(&g_dc);
    airtime_sched_init();

    /* Node identity */
    derive_node_id();
    g_net_id = (uint16_t)RIVR_NET_ID;
    strncpy(g_callsign, RIVR_CALLSIGN, sizeof(g_callsign) - 1u);
    g_callsign[sizeof(g_callsign) - 1u] = '\0';

    /* Subsystems */
    rivr_policy_init();
    rivr_fabric_init();
    rivr_embed_init();
    rivr_bus_init();

    build_info_print_banner();

#if RIVR_ROLE_CLIENT
    rivr_cli_init();
#endif

    /* Start receiving */
    radio_start_rx();

    RIVR_LOGI(TAG, "entering main loop");

    uint32_t last_stats_ms = 0;
    uint32_t loop_count    = 0;

    while (s_running) {
        uint32_t now = tb_millis();

#if RIVR_ROLE_CLIENT
        rivr_cli_poll();
#endif

        radio_service_rx();
        radio_check_timeouts();
        rivr_tick();
        tx_drain_loop();

        rivr_fabric_tick(now);

        /* Hot-reload after OTA program push */
        if (g_program_reload_pending) {
            rivr_embed_reload();
        }

        /* Periodic stats */
        if (now - last_stats_ms >= STATS_INTERVAL_MS) {
            last_stats_ms = now;
            RIVR_LOGI(TAG, "── stats (loop#%lu, uptime=%lu ms) ──",
                     (unsigned long)loop_count, (unsigned long)now);
            RIVR_LOGI(TAG, "  rx_ringbuf available : %u", rb_available(&rf_rx_ringbuf));
            RIVR_LOGI(TAG, "  tx_queue  available  : %u", rb_available(&rf_tx_queue));
            rivr_embed_print_stats();
        }

        loop_count++;

        /* 1-ms yield */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&ts, NULL);
    }

    RIVR_LOGI(TAG, "shutdown requested — exiting");
    return 0;
}
