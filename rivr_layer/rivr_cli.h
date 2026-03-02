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

#if RIVR_ROLE_CLIENT

/**
 * @brief Initialise the serial CLI.
 *
 * Installs the UART driver on UART_NUM_0 (if not already installed) so
 * rivr_cli_poll() can use uart_read_bytes() for non-blocking input.
 * Prints the boot banner: "Rivr Client Node Ready / Type 'help' for commands".
 *
 * Call once from app_main() after rivr_embed_init().
 */
void rivr_cli_init(void);

/**
 * @brief Non-blocking UART poll — handle one complete input line if available.
 *
 * Drains UART0 RX bytes into an internal 128-byte line buffer.
 * On newline (\n or \r): parses and executes the command, prints "> " prompt.
 * Supports backspace/DEL editing.  Returns immediately when no bytes pending.
 *
 * Supported commands:
 *   chat <message>  — broadcast PKT_CHAT; prints "TX CHAT: <message>"
 *   id              — print this node's 32-bit ID
 *   help            — print command list
 *
 * MUST be called BEFORE rivr_tick() each loop iteration so this function
 * consumes the UART bytes before sources_cli_drain() sees them.
 */
void rivr_cli_poll(void);

/**
 * @brief Display a received PKT_CHAT frame on the serial console.
 *
 * Prints: "\r[CHAT][XXXXXXXX]: <message>\n> "
 * The \r + > re-prompt keeps the display clean even if the user is mid-type.
 *
 * Call from rivr_sources.c (sources_rf_rx_drain) after successfully decoding
 * a PKT_CHAT frame, guarded by #if RIVR_ROLE_CLIENT.
 *
 * @param src_id   Source node ID from the packet header.
 * @param payload  Payload bytes (NOT NUL-terminated).
 * @param len      Payload length in bytes.
 */
void rivr_cli_on_chat_rx(uint32_t src_id, const uint8_t *payload, uint8_t len);

#else   /* !RIVR_ROLE_CLIENT — zero-cost stubs */

static inline void rivr_cli_init(void) {}
static inline void rivr_cli_poll(void) {}
static inline void rivr_cli_on_chat_rx(uint32_t src_id,
                                        const uint8_t *payload,
                                        uint8_t len)
{
    (void)src_id; (void)payload; (void)len;
}

#endif  /* RIVR_ROLE_CLIENT */

#ifdef __cplusplus
}
#endif

#endif /* RIVR_CLI_H */
