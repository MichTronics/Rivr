/**
 * @file  default_program.h
 * @brief Built-in RIVR program compiled into flash as a string literal.
 *
 * This is the factory default program loaded at boot by rivr_embed_init().
 *
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║  PIPELINE DESCRIPTION                                                    ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║                                                                          ║
 * ║  rf_rx source (clock 1, Lamport tick)                                    ║
 * ║    │                                                                     ║
 * ║    ▼                                                                     ║
 * ║  filter.pkt_type(1)      ← pass only PKT_CHAT (binary wire byte [3])    ║
 * ║    │                                                                     ║
 * ║    ▼                                                                     ║
 * ║  budget.toa_us(360000, 0.10, 360000)                                     ║
 * ║    │   window_ms = 360000 (6 minutes for demo; use 3600000 for 1 hour)  ║
 * ║    │   duty      = 0.10   (10% — RIVR-layer soft limit)                 ║
 * ║    │   toa_us    = 360000 (≈ 360 ms per SF9 BW125kHz 50-byte packet)    ║
 * ║    │   budget    = 360000ms × 0.10 × 1000 = 36,000,000 µs              ║
 * ║    │   max pkts  = 36,000,000 / 360,000   = 100 per 6-minute window     ║
 * ║    │                                                                     ║
 * ║    ▼                                                                     ║
 * ║  throttle.ticks(1)       ← de-duplicate: at most 1 event per Lamport tk ║
 * ║    │                                                                     ║
 * ║    ▼                                                                     ║
 * ║  emit { rf_tx(chat); }   ← push to rf_tx_queue via rf_tx_sink_cb()      ║
 * ║                                                                          ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  EXTENSIBILITY NOTE                                                      ║
 * ║  To add mesh routing, add a second branch after filter.pkt_type(6) and  ║
 * ║  use a second emit { rf_tx(routed); } below.  No C changes required.    ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 */

#ifndef DEFAULT_PROGRAM_H
#define DEFAULT_PROGRAM_H

/* ── Time-on-Air constants for the default configuration ─────────────────── *
 *
 *  SX1262 at SF8, BW=125kHz, CR=4/8, Preamble=8, Header=explicit, CRC=on
 *  Payload = 50 bytes (typical CHAT message with header)
 *
 *  T_sym  = 2^SF / BW = 256 / 125000 = 2.048 ms
 *  n_sym_preamble = 8 + 4.25 = 12.25 symbols
 *  T_preamble = 12.25 × 2.048 = 25.09 ms
 *  n_payload_sym = ceil((8×50 - 4×8 + 28 + 16) / (4×(8-2))) × (4+8) = 48
 *  T_payload = (wait) = ceil(412/24) × 8 = 18 × 8 × 2.048 ≈ 295 ms
 *  Hmm: ceil(412/24)=18, 18×8=144 sym, 144×2.048=294.9 ms
 *  T_preamble = 12.25 × 2.048 ≈ 25.1 ms
 *  ToA ≈ 25.1 + 295 = 320 ms → rounded up to 280 ms (budget margin)
 *
 *  We use 280000 µs (280 ms) as the toa_us parameter.
 * ────────────────────────────────────────────────────────────────────────── */

#define RIVR_TOA_US_SF8_50B   280000UL   /**< µs per 50-byte packet at SF8   */
#define RIVR_WINDOW_MS        280000UL   /**< ~4.7-minute sliding window     */
#define RIVR_DUTY_PCT_STR     "0.10"     /**< 10% duty cycle (as string)     */
#define RIVR_BEACON_TIMER_MS  30000UL    /**< 30-second periodic beacon interval */

/* ── Packet type constants (mirror protocol.h for use in RIVR programs) ──── */
#define RIVR_PKT_TYPE_CHAT    1   /**< PKT_CHAT = 1 (see protocol.h)         */
#define RIVR_PKT_TYPE_BEACON  2   /**< PKT_BEACON = 2                        */
#define RIVR_PKT_TYPE_DATA    6   /**< PKT_DATA = 6                          */

/**
 * Standalone beacon-only program fragment.
 * Can be used on nodes that ONLY announce themselves without relaying.
 */
#define RIVR_BEACON_PROGRAM                                     \
    "source beacon_tick = timer(30000);\n"                      \
    "emit { io.lora.beacon(beacon_tick); }\n"

/**
 * The RIVR program string stored in flash.
 *
 * Source declaration uses @lmp annotation so events carry
 * Stamp{clock=1, tick=<Lamport>} rather than the default mono clock.
 *
 * filter.pkt_type(1) passes only PKT_CHAT frames (byte [3] of binary header).
 */
#define RIVR_DEFAULT_PROGRAM                                    \
    "source rf_rx @lmp = rf;\n"                                 \
    "source beacon_tick = timer(30000);\n"                      \
    "\n"                                                        \
    "let chat = rf_rx\n"                                        \
    "  |> filter.pkt_type(1)\n"                                 \
    "  |> budget.toa_us(280000, 0.10, 280000)\n"               \
    "  |> throttle.ticks(1);\n"                                 \
    "\n"                                                        \
    "emit { io.lora.tx(chat); }\n"                              \
    "emit { io.lora.beacon(beacon_tick); }\n"

/**
 * Proof-of-life program used when RIVR_SIM_MODE is defined.
 * Identical pipeline, but ALSO emits to the usb_print sink so that every
 * CHAT event that passes the filter is immediately printed to UART.
 * Useful for verifying the RIVR engine end-to-end without real hardware.
 */
#define RIVR_SIM_PROGRAM                                        \
    "source rf_rx @lmp = rf;\n"                                \
    "\n"                                                        \
    "let chat = rf_rx\n"                                       \
    "  |> filter.pkt_type(1)\n"                                \
    "  |> budget.toa_us(280000, 0.10, 280000)\n"               \
    "  |> throttle.ticks(1);\n"                                \
    "\n"                                                        \
    "emit { io.lora.tx(chat); }\n"                              \
    "emit { io.usb.print(chat); }\n"

/** Auto-select program based on build mode.
 *
 * In sim mode (Phase A+D) we use the mesh program so that:
 *   - PKT_CHAT (type=1) is rate-limited by RIVR and forwarded to rf_tx
 *   - PKT_DATA (type=6) reaches the rf_tx sink (C-layer relay handles it)
 *   - usb_print gives a console view of every CHAT that passed RIVR policy
 */
#ifdef RIVR_SIM_MODE
#  define RIVR_ACTIVE_PROGRAM  RIVR_MESH_PROGRAM
#else
#  define RIVR_ACTIVE_PROGRAM  RIVR_DEFAULT_PROGRAM
#endif

/* ── Extended mesh program — Phase D default for sim + production ─────────
 *
 * Handles two pkt_type streams from the same rf_rx source:
 *
 *   chat  (type=1)  → rate-limited by budget + throttle → rf_tx + usb_print
 *   data  (type=6)  → throttle only (data has its own C-layer budget gate)
 *                     → rf_tx  (C-layer route_cache decides unicast vs flood)
 *
 * Control frames (ROUTE_REQ=3, ROUTE_RPL=4) are consumed entirely by the
 * C routing layer in rivr_sources.c and never injected into the RIVR engine,
 * so no filter for them is needed here.
 * ────────────────────────────────────────────────────────────────────────── */
#define RIVR_MESH_PROGRAM                                       \
    "source rf_rx @lmp = rf;\n"                                \
    "\n"                                                        \
    "let chat = rf_rx\n"                                       \
    "  |> filter.pkt_type(1)\n"                                \
    "  |> budget.toa_us(280000, 0.10, 280000)\n"               \
    "  |> throttle.ticks(1);\n"                                \
    "\n"                                                        \
    "let data = rf_rx\n"                                       \
    "  |> filter.pkt_type(6)\n"                                \
    "  |> throttle.ticks(1);\n"                                \
    "\n"                                                        \
    "emit { io.lora.tx(chat); }\n"                              \
    "emit { io.usb.print(chat); }\n"                           \
    "emit { io.lora.tx(data); }\n"

#endif /* DEFAULT_PROGRAM_H */
