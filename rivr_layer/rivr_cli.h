/**
 * @file  rivr_cli.h
 * @brief Serial CLI chat interface for the client node variant.
 *
 * Enabled only when RIVR_ROLE_CLIENT == 1.  In all other builds every
 * function is a zero-cost inline no-op so including this header is safe
 * everywhere.
 *
 * USAGE (main.c)
 * ──────────────
 *   rivr_cli_init();            // once, after rivr_embed_init()
 *   // main loop — MUST be before rivr_tick() to own UART bytes first:
 *   rivr_cli_poll();
 *
 * USAGE (rivr_sources.c — chat RX display)
 * ─────────────────────────────────────────
 *   rivr_cli_on_chat_rx(src_id, payload_ptr, payload_len);
 */

#ifndef RIVR_CLI_H
#define RIVR_CLI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if RIVR_ROLE_CLIENT || RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY

/**
 * @brief Initialise the serial CLI.
 *
 * Installs the UART driver on UART_NUM_0 (if not already installed) so
 * rivr_cli_poll() can use uart_read_bytes() for non-blocking input.
 * Prints the boot banner.  Call once from app_main() after rivr_embed_init().
 */
void rivr_cli_init(void);

/**
 * @brief Non-blocking UART poll — handle one complete input line if available.
 *
 * Drains UART0 RX bytes into an internal 128-byte line buffer.
 * On newline (\n or \r): parses and executes the command, prints "> " prompt.
 * Supports backspace/DEL editing.  Returns immediately when no bytes pending.
 *
 * MUST be called BEFORE rivr_tick() each loop iteration so this function
 * consumes the UART bytes before sources_cli_drain() sees them.
 */
void rivr_cli_poll(void);

/**
 * @brief Display a received PKT_CHAT frame on the serial console.
 *
 * Client builds only — no-op inline stub in repeater/gateway builds.
 *
 * @param src_id   Source node ID from the packet header.
 * @param payload  Payload bytes (NOT NUL-terminated).
 * @param len      Payload length in bytes.
 */
#  if RIVR_ROLE_CLIENT
void rivr_cli_on_chat_rx(uint32_t src_id, const uint8_t *payload, uint8_t len);

/**
 * @brief Enqueue a broadcast PKT_CHAT frame for transmission.
 *
 * Called from rivr_ble_companion.c when the companion app sends a
 * SEND_CHAT (0x08) CP command over the binary serial (SLIP) path.
 * Identical to typing 'chat <text>' on the serial CLI.
 *
 * @param text  UTF-8 message bytes (not NUL-terminated).
 * @param len   Number of bytes to send (1–RIVR_PKT_MAX_PAYLOAD).
 */
void rivr_cli_enqueue_chat_binary(const uint8_t *text, uint8_t len);
#  else
static inline void rivr_cli_on_chat_rx(uint32_t src_id,
                                        const uint8_t *payload,
                                        uint8_t len)
{ (void)src_id; (void)payload; (void)len; }
static inline void rivr_cli_enqueue_chat_binary(const uint8_t *text, uint8_t len)
{ (void)text; (void)len; }
#  endif

#else   /* no matching role — zero-cost stubs */

static inline void rivr_cli_init(void) {}
static inline void rivr_cli_poll(void) {}
static inline void rivr_cli_on_chat_rx(uint32_t src_id,
                                        const uint8_t *payload,
                                        uint8_t len)
{
    (void)src_id; (void)payload; (void)len;
}

#endif  /* RIVR_ROLE_CLIENT || RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY */

#ifdef __cplusplus
}
#endif

#endif /* RIVR_CLI_H */
