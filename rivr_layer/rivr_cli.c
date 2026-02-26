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
 *   chat <message>  broadcast a PKT_CHAT frame to all mesh nodes
 *   id              print this node's 32-bit identifier
 *   help            print command list
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

#ifdef RIVR_ROLE_CLIENT

#include "rivr_cli.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "driver/uart.h"
#include "esp_log.h"

#include "rivr_embed.h"
#include "../firmware_core/protocol.h"
#include "../firmware_core/radio_sx1262.h"
#include "../firmware_core/ringbuf.h"
#include "../firmware_core/timebase.h"

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
           "Node ID : 0x%08lX\r\n"
           "Net ID  : 0x%04X\r\n"
           "Commands: chat <msg> | id | help\r\n",
           (unsigned long)g_my_node_id,
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
               "  chat <message>   broadcast text to the mesh\r\n"
               "  id               print this node's 32-bit ID\r\n"
               "  help             show this list\r\n");
        fflush(stdout);
        return;
    }

    /* ── "id" ── */
    if (strncmp(p, "id", 2u) == 0 && (p[2] == '\0' || p[2] == ' ')) {
        printf("Node ID : 0x%08lX  Net ID : 0x%04X\r\n",
               (unsigned long)g_my_node_id, (unsigned)g_net_id);
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
    hdr.seq         = ++g_ctrl_seq;
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

    ESP_LOGI(TAG, "TX CHAT queued: \"%.*s\" (len=%u seq=%lu)",
             (int)len, msg, (unsigned)len, (unsigned long)hdr.seq);
    printf("TX CHAT: %.*s\r\n", (int)len, msg);
    fflush(stdout);
}

#endif /* RIVR_ROLE_CLIENT */
