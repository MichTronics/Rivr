/**
 * @file  rivr_cli.c
 * @brief Serial CLI chat interface for the client node variant.
 *
 * ARCHITECTURE
 * ────────────
 * Enabled only when RIVR_ROLE_CLIENT == 1 (client_esp32devkit_e22_900 env).
 * All other build variants compile an empty translation unit.
 *
 * rivr_cli_init()  — installs the UART0 driver (if not already installed by
 *                    another subsystem, e.g. SIM mode) and prints the boot
 *                    banner.  Call once from app_main() after rivr_embed_init().
 *
 * rivr_cli_poll()  — non-blocking UART0 drain.  Echo chars, handle backspace,
 *                    dispatch a complete command on newline.
 *                    MUST be called BEFORE rivr_tick() each main-loop iteration
 *                    so that this function owns the UART bytes before
 *                    sources_cli_drain() (called from rivr_tick()) can read
 *                    them.  sources_cli_drain() is a no-op in non-SIM hardware
 *                    builds, but the ordering guarantee prevents any future
 *                    race condition.
 *
 * rivr_cli_on_chat_rx() — display a received PKT_CHAT frame on the serial
 *                         console.  Called from sources_rf_rx_drain() in
 *                         rivr_sources.c guarded by #if RIVR_ROLE_CLIENT.
 *
 * COMMANDS
 * ────────
 *   chat <message>        broadcast a PKT_CHAT frame to all mesh nodes
 *   id                    print this node's 32-bit ID, callsign and net ID
 *   set callsign <CS>     set and persist callsign (max 11 chars, A-Z a-z 0-9 -)
 *   set netid <HEX>       set and persist network ID (hex 0..FFFF)
 *   status                print role, node ID, routing snapshot, loop-guard drops
 *   help                  print command list
 *
 * TX PATH
 * ───────
 *   Build rivr_pkt_hdr_t → protocol_encode() → rb_try_push(&rf_tx_queue, &req)
 *   Identical to the relay path in rivr_sources.c.
 *
 * UART DRIVER OWNERSHIP
 * ─────────────────────
 * The ESP-IDF VFS console (CONFIG_ESP_CONSOLE_UART_DEFAULT=y) routes printf /
 * ESP_LOG* output to UART0 via a thin write wrapper that does NOT install the
 * interrupt-driven uart_driver.  This file calls uart_driver_install() for the
 * client build.  uart_is_driver_installed() is checked first so the function is
 * idempotent (safe to call in a future unified init path).
 */

#if RIVR_ROLE_CLIENT

#include "rivr_cli.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>   /* strtoul */

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"        /* esp_restart()  */
#include "freertos/FreeRTOS.h" /* vTaskDelay()   */
#include "freertos/task.h"     /* pdMS_TO_TICKS  */
#include "../firmware_core/rivr_log.h"

#include "rivr_embed.h"
#include "../firmware_core/protocol.h"
#include "../firmware_core/radio_sx1262.h"
#include "../firmware_core/ringbuf.h"
#include "../firmware_core/timebase.h"
#include "../firmware_core/build_info.h"
#include "../firmware_core/rivr_metrics.h"
#include "../firmware_core/dutycycle.h"
#include "../firmware_core/rivr_policy.h"
#include "../firmware_core/routing_stats.h"
#include "../firmware_core/ble/rivr_ble.h"

/* ─── Constants ─────────────────────────────────────────────────────────── */

#define TAG              "CLI"
#define CLI_UART_PORT    UART_NUM_0
#define CLI_RX_BUF      512u    /**< UART RX ring-buffer size (bytes)        */
#define CLI_LINE_MAX     128u   /**< Max input line length including NUL      */

/* Maximum text payload that fits in a single PKT_CHAT frame.                 */
#define CLI_MSG_MAX      RIVR_PKT_MAX_PAYLOAD   /* 231 bytes                 */

/* ─── Module state ───────────────────────────────────────────────────────── */

static char    s_buf[CLI_LINE_MAX];   /**< Current input line buffer          */
static uint8_t s_pos = 0u;           /**< Write cursor inside s_buf          */

/* ─── Forward declarations ───────────────────────────────────────────────── */

static void cli_handle_line(void);
static void cli_enqueue_chat(const char *msg, size_t len);
static void cli_print_prompt(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void rivr_cli_init(void)
{
    /* Install the interrupt-driven UART driver so uart_read_bytes() works in
     * rivr_cli_poll().  Check first — SIM mode may have already installed it. */
    if (!uart_is_driver_installed(CLI_UART_PORT)) {
        const uart_config_t cfg = {
            .baud_rate           = 115200,
            .data_bits           = UART_DATA_8_BITS,
            .parity              = UART_PARITY_DISABLE,
            .stop_bits           = UART_STOP_BITS_1,
            .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .source_clk          = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_param_config(CLI_UART_PORT, &cfg));
        ESP_ERROR_CHECK(uart_driver_install(CLI_UART_PORT,
                                             (int)CLI_RX_BUF, 0,
                                             0, NULL, 0));
    }

    /* Boot banner */
    printf("\r\n"
           "╔══════════════════════════════════╗\r\n"
           "║   Rivr Client Node — Serial CLI  ║\r\n"
           "╚══════════════════════════════════╝\r\n"
           "Node ID  : 0x%08lX\r\n"
           "Callsign : %s\r\n"
           "Net ID   : 0x%04X\r\n"
           "Type 'help' for full command list.\r\n",
           (unsigned long)g_my_node_id,
           g_callsign,
           (unsigned)g_net_id);
    cli_print_prompt();
}

void rivr_cli_poll(void)
{
    uint8_t ch;

    /* Drain all available bytes until the RX FIFO is empty.                  */
    while (uart_read_bytes(CLI_UART_PORT, &ch, 1, 0) == 1) {

        if (ch == '\n' || ch == '\r') {
            /* ── End of line: execute command ── */
            printf("\r\n");
            fflush(stdout);
            s_buf[s_pos] = '\0';
            if (s_pos > 0u) {
                cli_handle_line();
            }
            s_pos = 0u;
            cli_print_prompt();

        } else if (ch == 0x08u || ch == 0x7Fu) {
            /* ── Backspace / DEL: erase last char ── */
            if (s_pos > 0u) {
                s_pos--;
                /* Visual backspace: overwrite with space then back again */
                printf("\b \b");
                fflush(stdout);
            }

        } else if (ch >= 0x20u && ch < 0x7Fu) {
            /* ── Printable ASCII: buffer and echo ── */
            if (s_pos < (CLI_LINE_MAX - 1u)) {
                s_buf[s_pos++] = (char)ch;
                putchar((int)ch);
                fflush(stdout);
            }
            /* Silently drop chars beyond CLI_LINE_MAX − 1 to prevent overflow */
        }
    }
}

void rivr_cli_on_chat_rx(uint32_t src_id, const uint8_t *payload, uint8_t len)
{
    /* Suppress self-echo: when the repeater relays our own CHAT frame back
     * over the air we hear it here.  This is correct mesh behaviour but
     * confusing on the CLI — the user already saw "TX CHAT: <msg>" when
     * they sent it.                                                          */
    if (src_id == g_my_node_id) return;

    /* Safely copy payload into a NUL-terminated scratch buffer.              */
    char txt[CLI_MSG_MAX + 1u];
    uint8_t copy_len = (len < CLI_MSG_MAX) ? len : CLI_MSG_MAX;
    memcpy(txt, payload, copy_len);
    txt[copy_len] = '\0';

    /* \r clears a partial "> " prompt line so the message prints cleanly.   */
    printf("\r[CHAT][%08lX]: %s\r\n",
           (unsigned long)src_id, txt);
    fflush(stdout);

    /* Re-print the prompt so the user sees where to type next.              */
    cli_print_prompt();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Private helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cli_print_prompt(void)
{
    /* Reprint the partial line so the user can continue editing after an
     * incoming CHAT message clears the line with \r.                         */
    printf("> ");
    if (s_pos > 0u) {
        /* Re-display whatever the user had typed so far */
        fwrite(s_buf, 1u, s_pos, stdout);
    }
    fflush(stdout);
}

/**
 * @brief Parse and dispatch a complete input line (NUL-terminated, no newline).
 *
 * Modifies s_buf in-place (adds NUL after token during parsing).
 */
static void cli_handle_line(void)
{
    /* Skip leading whitespace */
    char *p = s_buf;
    while (*p == ' ') { p++; }

    if (*p == '\0') {
        /* Empty line — harmless */
        return;
    }

    /* ── "help" ── */
    if (strncmp(p, "help", 4u) == 0 && (p[4] == '\0' || p[4] == ' ')) {
        printf("Commands:\r\n"
               "  chat <message>        broadcast text to the mesh\r\n"
               "  send <message>        alias for chat\r\n"
               "  id                    print node ID, callsign and net ID\r\n"
               "  info                  print build info (env, sha, radio profile)\r\n"
               "  status                role, node ID, routing snapshot, loop-guard drops\r\n"
               "  neighbors             show live neighbour table with link scores\r\n"
               "  peers                 alias for neighbors\r\n"
               "  routes                show route cache with scores and ages\r\n"
               "  metrics               print all counters/gauges as JSON (@MET line)\r\n"
               "  stats                 alias for metrics\r\n"
               "  policy                print current policy params + counters (@POLICY line)\r\n"
               "  supportpack           JSON dump: build info + metrics snapshot\r\n"
               "  ntable               standalone quality table: RSSI/SNR/ETX×8/loss per peer\r\n"
               "  fwdset               forward-candidate set: relay quality + holdoff class\r\n"
               "  rtstats               routing pipeline telemetry snapshot (@RST JSON block)\r\n"
               "  set callsign <CS>     set and persist callsign (1-11 chars: A-Z a-z 0-9 -)\r\n"
               "  set netid <HEX>       set and persist network ID (hex 0..FFFF)\r\n"
               "  log <debug|metrics|silent>  set log verbosity\r\n"
               "  ble-clear-bonds       remove all persisted BLE bonds from this node\r\n"
               "  reboot                software reset the device\r\n"
               "  help                  show this list\r\n");
        fflush(stdout);
        return;
    }

    /* ── "id" ── */
    if (strncmp(p, "id", 2u) == 0 && (p[2] == '\0' || p[2] == ' ')) {
        printf("Node ID  : 0x%08lX\r\nCallsign : %s\r\nNet ID   : 0x%04X\r\n",
               (unsigned long)g_my_node_id, g_callsign, (unsigned)g_net_id);
        fflush(stdout);
        return;
    }

    /* ── "set callsign <CS>" ── */
    if (strncmp(p, "set callsign", 12u) == 0 && (p[12] == ' ' || p[12] == '\0')) {
        char *arg = p + 12;
        while (*arg == ' ') { arg++; }
        /* Strip trailing whitespace */
        size_t clen = strlen(arg);
        while (clen > 0u && arg[clen - 1u] == ' ') { clen--; }
        arg[clen] = '\0';
        if (clen == 0u || clen > 11u) {
            printf("ERR: callsign must be 1-11 characters\r\n");
            fflush(stdout);
            return;
        }
        /* Validate: A-Z a-z 0-9 dash only */
        for (size_t i = 0u; i < clen; i++) {
            char c = arg[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-')) {
                printf("ERR: callsign may only contain A-Z a-z 0-9 -\r\n");
                fflush(stdout);
                return;
            }
        }
        strncpy(g_callsign, arg, sizeof(g_callsign) - 1u);
        g_callsign[sizeof(g_callsign) - 1u] = '\0';
        if (!rivr_nvs_store_identity(g_callsign, g_net_id)) {
            printf("WARN: NVS write failed – callsign updated for this session only\r\n");
        } else {
            printf("OK callsign set to %s\r\n", g_callsign);
        }
        fflush(stdout);
        return;
    }

    /* ── "set netid <HEX>" ── */
    if (strncmp(p, "set netid", 9u) == 0 && (p[9] == ' ' || p[9] == '\0')) {
        char *arg = p + 9;
        while (*arg == ' ') { arg++; }
        if (*arg == '\0') {
            printf("ERR: usage: set netid <hex>\r\n");
            fflush(stdout);
            return;
        }
        char *end = NULL;
        unsigned long nid = strtoul(arg, &end, 16);
        if (end == arg || nid > 0xFFFFu) {
            printf("ERR: net ID must be a hex value 0..FFFF\r\n");
            fflush(stdout);
            return;
        }
        g_net_id = (uint16_t)nid;
        if (!rivr_nvs_store_identity(g_callsign, g_net_id)) {
            printf("WARN: NVS write failed – net ID updated for this session only\r\n");
        } else {
            printf("OK net ID set to 0x%04X\r\n", (unsigned)g_net_id);
        }
        fflush(stdout);
        return;
    }

    /* ── "chat <message>" ── */
    if (strncmp(p, "chat", 4u) == 0 && (p[4] == ' ' || p[4] == '\0')) {
        char *msg = p + 4;
        /* Skip whitespace between "chat" and the message text */
        while (*msg == ' ') { msg++; }
        if (*msg == '\0') {
            printf("ERR: usage: chat <message>\r\n");
            fflush(stdout);
            return;
        }
        cli_enqueue_chat(msg, strlen(msg));
        return;
    }

    /* ── "info" ── */
    if (strncmp(p, "info", 4u) == 0 && (p[4] == '\0' || p[4] == ' ')) {
        build_info_print_banner();
        fflush(stdout);
        return;
    }

    /* ── "metrics" ── */
    if (strncmp(p, "metrics", 7u) == 0 && (p[7] == '\0' || p[7] == ' ')) {
        uint32_t now_ms = tb_millis();
        {
            const neighbor_link_summary_t _lnk =
                neighbor_table_link_summary(&g_ntable, now_ms);
            const rivr_live_stats_t ls = {
                .node_id       = g_my_node_id,
                .dc_pct        = (uint8_t)(DC_BUDGET_US > 0u
                                 ? ((DC_BUDGET_US - dutycycle_remaining_us(&g_dc)) * 100ULL
                                    / DC_BUDGET_US)
                                 : 0u),
                .q_depth       = (uint8_t)rb_available(&rf_tx_queue),
                .tx_total      = g_tx_frame_count,
                .rx_total      = g_rx_frame_count,
                .route_cache   = route_cache_count(&g_route_cache, now_ms),
                .lnk_cnt       = _lnk.count,
                .lnk_best      = _lnk.best_score,
                .lnk_best_rssi = _lnk.best_rssi,
                .lnk_avg_loss  = _lnk.avg_loss,
                .relay_density = _lnk.count,
            };
            rivr_metrics_print(&ls);
        }
        fflush(stdout);
        return;
    }

    /* ── "supportpack" ── */
    if (strncmp(p, "supportpack", 11u) == 0 && (p[11] == '\0' || p[11] == ' ')) {
        uint32_t now_ms = tb_millis();
        char sp_buf[768];
        int n = build_info_write_supportpack(
            sp_buf, sizeof(sp_buf),
            routing_neighbor_count(&g_neighbor_table, now_ms),
            route_cache_count(&g_route_cache, now_ms),
            pending_queue_count(&g_pending_queue),
            dutycycle_remaining_us(&g_dc),
            g_dc.used_us,
            g_dc.blocked_count,
            now_ms,
            g_rx_frame_count,
            g_tx_frame_count);
        printf("@SUPPORTPACK %.*s\r\n", n, sp_buf);
        fflush(stdout);
        return;
    }

    /* ── "log <mode>" ── */
    if (strncmp(p, "log", 3u) == 0 && (p[3] == ' ' || p[3] == '\0')) {
        char *arg = p + 3;
        while (*arg == ' ') { arg++; }
        if (strcmp(arg, "debug") == 0) {
            rivr_log_set_mode(RIVR_LOG_DEBUG);
            printf("OK log mode: debug\r\n");
        } else if (strcmp(arg, "metrics") == 0) {
            rivr_log_set_mode(RIVR_LOG_METRICS);
            printf("OK log mode: metrics\r\n");
        } else if (strcmp(arg, "silent") == 0) {
            rivr_log_set_mode(RIVR_LOG_SILENT);
            printf("OK log mode: silent\r\n");
        } else {
            printf("ERR: usage: log <debug|metrics|silent>\r\n");
        }
        fflush(stdout);
        return;
    }

    /* ── "policy" ── */
    if (strncmp(p, "policy", 6u) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        rivr_policy_print();
        return;
    }

    /* ── "neighbors" ── */
    if (strncmp(p, "neighbors", 9u) == 0 && (p[9] == '\0' || p[9] == ' ')) {
        uint32_t now_ms = tb_millis();
        uint8_t  cnt    = routing_neighbor_count(&g_neighbor_table, now_ms);
        printf("Neighbors (%u live):\r\n", (unsigned)cnt);
        routing_neighbor_print(&g_neighbor_table, now_ms);
        fflush(stdout);
        return;
    }

    /* ── "routes" ── */
    if (strncmp(p, "routes", 6u) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        uint32_t now_ms = tb_millis();
        uint8_t  cnt    = route_cache_count(&g_route_cache, now_ms);
        printf("Routes (%u live):\r\n", (unsigned)cnt);
        route_cache_print(&g_route_cache, now_ms);
        fflush(stdout);
        return;
    }

    /* ── "status" ── */
    if (strncmp(p, "status", 6u) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        uint32_t now_ms  = tb_millis();
        uint8_t  nbrs    = routing_neighbor_count(&g_neighbor_table, now_ms);
        uint8_t  routes  = route_cache_count(&g_route_cache, now_ms);
        uint8_t  pending = pending_queue_count(&g_pending_queue);
        printf("Status:\r\n"
               "  role          : " _RIVR_ROLE_TAG "\r\n"
               "  node_id       : 0x%08lX\r\n"
               "  callsign      : %s\r\n"
               "  net_id        : 0x%04X\r\n"
               "  uptime_ms     : %lu\r\n"
               "  rx_frames     : %lu\r\n"
               "  tx_frames     : %lu\r\n"
               "  neighbors     : %u\r\n"
               "  routes        : %u / %u  (active / capacity)\r\n"
               "  pending       : %u\r\n"
               "  retry_cap     : %u\r\n"
               "  relay_budget  : %u fwd/type/min\r\n"
               "  rc_hit        : %lu\r\n"
               "  rc_miss       : %lu\r\n"
               "Routing metrics:\r\n"
               "  rreq_rx       : %lu\r\n"
               "  rreq_target   : %lu\r\n"
               "  rreq_cache    : %lu\r\n"
               "  rreq_suppress : %lu\r\n"
               "  rrpl_rx       : %lu\r\n"
               "  rrpl_learn    : %lu\r\n"
               "  fwd_ttl_drop  : %lu\r\n"
               "  pq_drained    : %lu\r\n"
               "  pq_expired    : %lu\r\n"
               "Retry / reliability:\r\n"
               "  ack_rx        : %lu\r\n"
               "  ack_tx        : %lu\r\n"
               "  retry_attempt : %lu\r\n"
               "  retry_ok      : %lu\r\n"
               "  retry_fail    : %lu\r\n"
               "  retry_fallback: %lu\r\n"
               "  flood_fallback: %lu\r\n"
               "Loop guard:\r\n"
               "  loop_drops    : %lu\r\n",
               (unsigned long)g_my_node_id,
               g_callsign,
               (unsigned)g_net_id,
               (unsigned long)now_ms,
               (unsigned long)g_rx_frame_count,
               (unsigned long)g_tx_frame_count,
               (unsigned)nbrs,
               (unsigned)routes,   (unsigned)RCACHE_SIZE,
               (unsigned)pending,
               (unsigned)RETRY_TABLE_SIZE,
               (unsigned)FWDBUDGET_MAX_FWD_ROLE,
               (unsigned long)g_rivr_metrics.route_cache_hit_total,
               (unsigned long)g_rivr_metrics.route_cache_miss_total,
               (unsigned long)g_rivr_metrics.route_req_rx_total,
               (unsigned long)g_rivr_metrics.route_req_reply_target_total,
               (unsigned long)g_rivr_metrics.route_req_reply_cache_total,
               (unsigned long)g_rivr_metrics.route_req_reply_suppressed_total,
               (unsigned long)g_rivr_metrics.route_rpl_rx_total,
               (unsigned long)g_rivr_metrics.route_rpl_learn_total,
               (unsigned long)g_rivr_metrics.forward_drop_ttl_total,
               (unsigned long)g_rivr_metrics.pending_queue_drained_total,
               (unsigned long)g_rivr_metrics.pending_queue_expired_total,
               (unsigned long)g_rivr_metrics.ack_rx_total,
               (unsigned long)g_rivr_metrics.ack_tx_total,
               (unsigned long)g_rivr_metrics.retry_attempt_total,
               (unsigned long)g_rivr_metrics.retry_success_total,
               (unsigned long)g_rivr_metrics.retry_fail_total,
               (unsigned long)g_rivr_metrics.retry_fallback_total,
               (unsigned long)g_rivr_metrics.fallback_flood_total,
               (unsigned long)g_rivr_metrics.loop_detect_drop_total);
        fflush(stdout);
        return;
    }

    /* ── "ntable" — standalone quality neighbor table with ETX×8 data ── */
    if (strncmp(p, "ntable", 6u) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        printf("Quality neighbors (%u entries):\r\n",
               (unsigned)g_ntable.count);
        neighbor_table_print(&g_ntable, tb_millis());
        fflush(stdout);
        return;
    }

    /* ── "fwdset" — Phase 5 forward-candidate set + relay quality gate ── */
    if (strncmp(p, "fwdset", 6u) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        uint32_t now_ms = tb_millis();
        fwd_set_t fs = fwdset_build(&g_ntable, now_ms);
        char buf[120];
        fwdset_sprint(&fs, buf, (uint8_t)sizeof(buf));
        printf("Forward set: %s\r\n"
               "  suppress=%s  extra_holdoff=%lu ms\r\n"
               "  score_suppressed_total=%lu\r\n",
               buf,
               fwdset_suppress_relay(&fs) ? "YES" : "no",
               (unsigned long)fwdset_extra_holdoff_ms(&fs),
               (unsigned long)g_rivr_metrics.flood_fwd_score_suppressed_total);
        fflush(stdout);
        return;
    }

    /* ── "rtstats" — Phase 0 routing pipeline telemetry snapshot ── */
    if (strncmp(p, "rtstats", 7u) == 0 && (p[7] == '\0' || p[7] == ' ')) {
        uint32_t now_ms = tb_millis();
        uint8_t  rc  = (uint8_t)route_cache_count(&g_route_cache, now_ms);
        uint8_t  nb  = (uint8_t)routing_neighbor_count(&g_neighbor_table, now_ms);
        uint8_t  pq  = (uint8_t)pending_queue_count(&g_pending_queue);
        uint8_t  dc  = (uint8_t)(DC_BUDGET_US > 0u
                        ? ((DC_BUDGET_US - dutycycle_remaining_us(&g_dc)) * 100ULL
                           / DC_BUDGET_US)
                        : 0u);
        rivr_routing_stats_t rs = rivr_routing_stats_collect(rc, nb, pq, dc, now_ms);
        rivr_routing_stats_print(&rs);
        fflush(stdout);
        return;
    }

    /* ── Aliases for convenience / discoverability ──────────────────────── */

    /* "send <message>" — alias for "chat <message>" */
    if (strncmp(p, "send", 4u) == 0 && (p[4] == ' ' || p[4] == '\0')) {
        char *msg = p + 4;
        while (*msg == ' ') { msg++; }
        if (*msg == '\0') {
            printf("ERR: usage: send <message>\r\n");
            fflush(stdout);
            return;
        }
        cli_enqueue_chat(msg, strlen(msg));
        return;
    }

    /* "peers" — alias for "neighbors" */
    if (strncmp(p, "peers", 5u) == 0 && (p[5] == '\0' || p[5] == ' ')) {
        uint32_t now_ms = tb_millis();
        uint8_t  cnt    = routing_neighbor_count(&g_neighbor_table, now_ms);
        printf("Neighbors (%u live):\r\n", (unsigned)cnt);
        routing_neighbor_print(&g_neighbor_table, now_ms);
        fflush(stdout);
        return;
    }

    /* "stats" — alias for "metrics" */
    if (strncmp(p, "stats", 5u) == 0 && (p[5] == '\0' || p[5] == ' ')) {
        uint32_t now_ms = tb_millis();
        {
            const neighbor_link_summary_t _lnk =
                neighbor_table_link_summary(&g_ntable, now_ms);
            const rivr_live_stats_t ls = {
                .node_id       = g_my_node_id,
                .dc_pct        = (uint8_t)(DC_BUDGET_US > 0u
                                 ? ((DC_BUDGET_US - dutycycle_remaining_us(&g_dc)) * 100ULL
                                    / DC_BUDGET_US)
                                 : 0u),
                .q_depth       = (uint8_t)rb_available(&rf_tx_queue),
                .tx_total      = g_tx_frame_count,
                .rx_total      = g_rx_frame_count,
                .route_cache   = route_cache_count(&g_route_cache, now_ms),
                .lnk_cnt       = _lnk.count,
                .lnk_best      = _lnk.best_score,
                .lnk_best_rssi = _lnk.best_rssi,
                .lnk_avg_loss  = _lnk.avg_loss,
                .relay_density = _lnk.count,
            };
            rivr_metrics_print(&ls);
        }
        fflush(stdout);
        return;
    }

    /* "ble-clear-bonds" — remove persisted BLE bond records */
    if (strncmp(p, "ble-clear-bonds", 15u) == 0 &&
        (p[15] == '\0' || p[15] == ' ')) {
        int removed = rivr_ble_clear_bonds();
        if (removed < 0) {
            printf("ERR: failed to clear BLE bonds\r\n");
        } else {
            printf("OK BLE bonds cleared: %d\r\n", removed);
            printf("If the phone was previously paired, forget the node in Android too.\r\n");
        }
        fflush(stdout);
        return;
    }

    /* "reboot" — software reset via esp_restart() */
    if (strncmp(p, "reboot", 6u) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        printf("Rebooting...\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));   /* allow UART TX buffer to flush */
        esp_restart();
        return; /* unreachable */
    }

    /* ── Unknown command ── */
    printf("ERR: unknown command '%s' — type 'help'\r\n", p);
    fflush(stdout);
}

/**
 * @brief Encode and enqueue a PKT_CHAT broadcast frame.
 *
 * @param msg  NUL-terminated message text (not modified).
 * @param len  strlen(msg) — MUST be > 0.
 */
static void cli_enqueue_chat(const char *msg, size_t len)
{
    if (len > (size_t)CLI_MSG_MAX) {
        printf("ERR: message too long (max %u bytes)\r\n", (unsigned)CLI_MSG_MAX);
        fflush(stdout);
        return;
    }

    /* ── Origination policy gate ────────────────────────────────────────── *
     * Rivr policy decides ALLOW/DENY only.  Frame construction and queuing  *
     * are unchanged — they only run when the gate passes.                   *
     * The throttle window is OTA-adjustable via "@PARAMS chat=<ms>".        *
     * ─────────────────────────────────────────────────────────────────────  */
    if (!rivr_policy_allow_origination(PKT_CHAT, tb_millis())) {
        printf("@DROP origination throttled type=CHAT\r\n");
        fflush(stdout);
        return;
    }

    /* Build packet header — identical pattern to the relay path in
     * rivr_sources.c so every PKT_CHAT has a consistent wire format.         */
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkt_type    = PKT_CHAT;
    hdr.flags       = 0u;
    hdr.ttl         = RIVR_PKT_DEFAULT_TTL;
    hdr.hop         = 0u;
    hdr.net_id      = g_net_id;
    hdr.src_id      = g_my_node_id;
    hdr.dst_id      = 0u;               /* broadcast */
    hdr.seq         = (uint16_t)++g_ctrl_seq;
    hdr.pkt_id      = (uint16_t)g_ctrl_seq;
    hdr.payload_len = (uint8_t)len;

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));

    int enc = protocol_encode(&hdr,
                               (const uint8_t *)msg, (uint8_t)len,
                               req.data, sizeof(req.data));
    if (enc <= 0) {
        ESP_LOGE(TAG, "protocol_encode failed (%d)", enc);
        printf("ERR: frame encoding failed\r\n");
        fflush(stdout);
        return;
    }

    req.len    = (uint8_t)enc;
    req.toa_us = RF_TOA_APPROX_US(req.len);
    req.due_ms = 0u;    /* send as soon as duty-cycle permits */

    if (!rb_try_push(&rf_tx_queue, &req)) {
        ESP_LOGW(TAG, "TX queue full — chat frame dropped");
        printf("ERR: TX queue full\r\n");
        fflush(stdout);
        return;
    }

    RIVR_LOGI(TAG, "TX CHAT queued: \"%.*s\" (len=%u seq=%lu)",
             (int)len, msg, (unsigned)len, (unsigned long)hdr.seq);
    printf("TX CHAT: %.*s\r\n", (int)len, msg);
    fflush(stdout);
}

#endif /* RIVR_ROLE_CLIENT */
