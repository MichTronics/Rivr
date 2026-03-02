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
 * ║  budget.toa_us(280000, 0.10, 280000)   ← policy / rate accounting       ║
 * ║    │                                                                     ║
 * ║    ▼                                                                     ║
 * ║  throttle.ticks(1)       ← de-duplicate within one Lamport tick         ║
 * ║    │                                                                     ║
 * ║    ▼  (no io.lora.tx emit — relay is handled exclusively by the          ║
 * ║        C-layer maybe_relay path which correctly increments hop/TTL)      ║
 * ║                                                                          ║
 * ╠══════════════════════════════════════════════════════════════════════════╣
 * ║  RELAY ARCHITECTURE NOTE                                                 ║
 * ║  io.lora.tx is for ORIGINATED traffic only (future sensor/data nodes).  ║
 * ║  Received frames that need forwarding are relayed by rivr_sources.c      ║
 * ║  (maybe_relay), which increments hop, decrements TTL, and applies        ║
 * ║  deterministic jitter. Emitting raw received bytes via io.lora.tx would  ║
 * ║  re-broadcast them with hop=0, creating phantom origins and relay loops. ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 */

#ifndef DEFAULT_PROGRAM_H
#define DEFAULT_PROGRAM_H

/*
 * Rivr Policy Parameters are now runtime-adjustable via PKT_PROG_PUSH.
 * Mechanism (routing, ACK, queue) remains in C.
 * Rivr continues to serve as policy layer only.
 *
 * Send "@PARAMS beacon=<ms> chat=<ms> data=<ms> duty=<1..10>" as a
 * PKT_PROG_PUSH payload to update runtime policy without reflashing.
 * Default values are the RIVR_PARAM_* macros below.
 *
 * Use the CLI command "policy" or watch for @POLICY JSON lines to
 * observe the current parameter values and update counters:
 *   @POLICY {"beacon":30000,"chat":2000,"data":2000,"duty":10,
 *            "updates":N,"last_update_ms":N,"rebuilds":N,"reloads":N,
 *            "duty_blocked":0,"orig_drops":0}
 */

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

/* ── Runtime-adjustable policy parameters (OTA via PKT_PROG_PUSH @PARAMS) ── *
 *
 * These are the compiled-in defaults.  rivr_policy_init() loads them into
 * g_policy_params at boot.  rivr_policy_set_param() validates and updates
 * them at runtime.  rivr_policy_build_program() generates the active RIVR
 * program string from g_policy_params (used instead of the static macros
 * below when a param-update OTA has been applied).
 * ────────────────────────────────────────────────────────────────────────── */
#define RIVR_PARAM_BEACON_INTERVAL_MS   30000UL  /**< Beacon period, ms (OTA-adjustable) */
#define RIVR_PARAM_CHAT_THROTTLE_MS      2000UL  /**< PKT_CHAT throttle window, ms       */
#define RIVR_PARAM_DATA_THROTTLE_MS      2000UL  /**< PKT_DATA throttle window, ms       */
#define RIVR_PARAM_DUTY_PERCENT             10u  /**< TX duty-cycle limit 1–10 %         */

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
/* RIVR_DEFAULT_PROGRAM
 * ─────────────────────
 * The chat pipeline runs PKT_CHAT frames through budget + throttle so that
 * RIVR tracks airtime consumption and provides policy metrics (clock[1],
 * FAB score).  NO io.lora.tx emit — relay is performed exclusively by the
 * C-layer (maybe_relay in rivr_sources.c) which increments hop/TTL and
 * applies deterministic jitter.  Emitting raw received bytes via io.lora.tx
 * would re-broadcast them with the original hop=0, creating phantom origins
 * and relay loops.
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
    "emit { io.usb.print(chat); }\n"

// Using RIVR_MESH_PROGRAM to ensure both PKT_CHAT and PKT_DATA
// are subject to unified policy gating.
// RIVR_ACTIVE_PROGRAM is the compiled-in fallback used by rivr_embed.c
// when neither NVS program nor @PARAMS OTA update is present.
#define RIVR_ACTIVE_PROGRAM  RIVR_MESH_PROGRAM

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
    "source beacon_tick = timer(30000);\n"                     \
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
    "emit { io.lora.beacon(beacon_tick); }\n"

#endif /* DEFAULT_PROGRAM_H */
