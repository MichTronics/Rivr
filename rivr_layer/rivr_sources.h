/**
 * @file  rivr_sources.h
 * @brief RIVR event sources: rf_rx (LoRa radio), cli (UART), timer.
 *
 * Each source knows how to:
 *   1. Poll its hardware queue / ringbuffer for pending data.
 *   2. Convert raw data into a `rivr_event_t`.
 *   3. Inject the event into the RIVR engine via `rivr_inject_event()`.
 *
 * RIVR Source → Clock Mapping
 * ────────────────────────────
 *  rf_rx  → Clock 1 (Lamport logical tick, advanced per received packet)
 *  cli    → Clock 0 (monotonic millis)
 *  timer  → Clock 0 (monotonic millis, periodic)
 */

#ifndef RIVR_SOURCES_H
#define RIVR_SOURCES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "rivr_embed.h"

/* ── Drain limits (max events injected per rivr_tick() call) ─────────────── */
#define SOURCES_RF_RX_DRAIN_LIMIT  4u   /**< Max RX frames processed per tick  */
#define SOURCES_CLI_DRAIN_LIMIT    2u

/* ── rf_rx source ────────────────────────────────────────────────────────── */

/**
 * @brief Drain the rf_rx ringbuffer and inject events into RIVR.
 *
 * For each frame:
 *   1. Calls radio_decode_frame() → "CHAT:<text>" | "DATA:<hex>" etc.
 *   2. Extracts sender Lamport tick from frame header.
 *   3. Calls tb_lmp_advance(sender_tick) to advance the local Lamport clock.
 *   4. Constructs rivr_event_t with Stamp{clock=1, tick=assigned_tick}.
 *   5. Sets kind_tag to the frame type prefix ("CHAT", "DATA", etc.).
 *   6. Injects into the "rf_rx" RIVR source.
 *
 * @return number of events injected
 */
uint32_t sources_rf_rx_drain(void);

/* ── cli source ──────────────────────────────────────────────────────────── */

/**
 * @brief Read pending CLI input and inject "CLI:<text>" events into RIVR.
 *
 * Collects characters from the UART RX FIFO up to newline or 126 chars.
 * Stamp uses Clock 0 (monotonic millis).
 *
 * @return number of events injected (0 or 1 per call)
 */
uint32_t sources_cli_drain(void);

/* ── timer source ────────────────────────────────────────────────────────── */

/**
 * @brief Inject periodic timer events into the RIVR "timer" source.
 *
 * Fires at most once per `interval_ms` milliseconds.
 * Event value: "TICK:<mono_ms>" (FixedText<128>).
 *
 * @param interval_ms  fire interval in milliseconds (e.g. 1000 for 1 Hz)
 * @return 1 if an event was injected this call, 0 otherwise
 */
uint32_t sources_timer_tick(uint32_t interval_ms);

/* ── Initialisation ──────────────────────────────────────────────────────── */

/** Register all sources with the embedded RIVR engine. */
void rivr_sources_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_SOURCES_H */
