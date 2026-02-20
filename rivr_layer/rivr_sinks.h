/**
 * @file  rivr_sinks.h
 * @brief RIVR emit sinks: rf_tx (push to TX queue), log (UART print).
 *
 * Sinks are registered as callbacks before rivr_engine_init() is called.
 * They are invoked from within rivr_engine_run() when RIVR executes an
 * `emit { ... }` statement.
 *
 * RIVR Sink → Hardware Mapping
 * ────────────────────────────
 *  rf_tx  → encodes the value back to a raw frame, pushes to rf_tx_queue
 *  log    → formats and prints to UART (ESP_LOGI)
 *
 * MEMORY RULES
 * ─────────────
 *  Sink callbacks MUST NOT allocate heap.  They write into pre-allocated
 *  rf_tx_request_t structs and push them (by value) into the SPSC tx_queue.
 */

#ifndef RIVR_SINKS_H
#define RIVR_SINKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "rivr_embed.h"

/* ── rf_tx sink ──────────────────────────────────────────────────────────── */

/**
 * @brief Emit callback: value → raw RF frame → rf_tx_queue.
 *
 * Encoding rules:
 *   Value::Str("CHAT:<text>")  → frame_type=0x04, payload = raw text
 *   Value::Bytes(raw)          → frame_type=0x01, payload = raw bytes
 *   Anything else              → dropped (logged as warning)
 *
 * Prepends the 3-byte header: [frame_type, lmp_tick_lo, lmp_tick_hi]
 * before the payload.
 *
 * Calculates ToA using RF_TOA_APPROX_US(payload_len) and records the
 * request in rf_tx_queue.
 *
 * @param v         emitted value from RIVR
 * @param user_ctx  unused (NULL)
 */
void rf_tx_sink_cb(const rivr_value_t *v, void *user_ctx);

/* ── log sink ────────────────────────────────────────────────────────────── */

/**
 * @brief Emit callback: value → ESP_LOGI formatted string.
 *
 * Converts the value to its string representation and logs it using the
 * ESP-IDF logging framework.  Tag: "RIVR".
 *
 * @param v         emitted value from RIVR
 * @param user_ctx  optional const char* prefix string (or NULL)
 */
void log_sink_cb(const rivr_value_t *v, void *user_ctx);

/* ── usb_print sink ──────────────────────────────────────────────────────── */

/**
 * @brief Emit callback: value → immediate printf() to UART (stdout).
 *
 * Unlike log_sink_cb (which uses ESP_LOGI with a timestamp prefix),
 * this sink writes a plain line that is easier to parse in CI / host tests:
 *   [RIVR-TX] <string>   for Str values
 *   [RIVR-TX] <int>      for Int values
 *
 * Registered as sink name "usb_print".
 *
 * @param v         emitted value from RIVR
 * @param user_ctx  unused (NULL)
 */
void usb_print_sink_cb(const rivr_value_t *v, void *user_ctx);

/* ── Initialisation ──────────────────────────────────────────────────────── */

/** Register all sinks with rivr_register_sink(). Call before rivr_embed_init(). */
void rivr_sinks_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_SINKS_H */
