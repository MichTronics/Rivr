/**
 * @file  feature_flags.h
 * @brief Consolidated compile-time feature flag definitions for RIVR firmware.
 *
 * This is the SINGLE SOURCE OF TRUTH for all RIVR compile-time feature knobs.
 * Each flag defaults to the safest/smallest value and can be overridden via:
 *   a) Variant config.h (variants/<board>/config.h, -include'd by platformio.ini)
 *   b) Direct -D flag in platformio.ini build_flags
 *
 * All flags use #ifndef guards so any earlier definition always wins.
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ FLAG                         │ DEFAULT │ DESCRIPTION                  │
 * ├──────────────────────────────────────────────────────────────────────┤
 * │ RIVR_ROLE_CLIENT             │  0      │ Node is a client (chat only) │
 * │ RIVR_ROLE_REPEATER           │  0      │ Node relays CHAT+DATA        │
 * │ RIVR_ROLE_GATEWAY            │  0      │ Gateway stub (future IP      │
 * │                              │         │ bridge — no active relay)    │
 * │ RIVR_FABRIC_REPEATER         │  0      │ Congestion fabric enabled    │
 * │ RIVR_RADIO_SX1262            │  1      │ Use SX1262/E22 driver        │
 * │ RIVR_RADIO_SX1276            │  0      │ Use SX1276/RFM95 driver      │
 * │ RIVR_FEATURE_ENCRYPTION      │  0      │ PSK frame encryption         │
 * │ RIVR_FEATURE_COMPRESSION     │  0      │ Payload compression (future) │
 * │ RIVR_FEATURE_BLE             │  0      │ BLE provisioning (future)    │
 * │ RIVR_SIGNED_PROG             │  0      │ OTA requires Ed25519 sig     │
 * │ RIVR_FEATURE_SIGNED_PARAMS   │  0      │ @PARAMS require HMAC-SHA-256 │
 * │ RIVR_FEATURE_ALLOW_UNSIGNED  │  1      │ Accept unsigned @PARAMS when │
 * │   _PARAMS                    │         │ SIGNED_PARAMS=1 (dev grace)  │
 * │ RIVR_PARAMS_PSK_HEX          │ "000…"  │ 64-hex 32-byte HMAC key      │
 * │ RIVR_SIM_MODE                │  0      │ Simulation / host-test build │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * ROLE GUARD
 * ──────────
 * Exactly one of RIVR_ROLE_CLIENT / RIVR_ROLE_REPEATER should be set per
 * build.  The guard at the bottom of this file emits a compile-time warning
 * when neither is set (generic fallback — valid for tests/sim).
 *
 * RADIO GUARD
 * ───────────
 * Exactly one of RIVR_RADIO_SX1262 / RIVR_RADIO_SX1276 must be set.
 * A static_assert fires if neither is set in a non-simulation build.
 */

#pragma once

/* ── Role flags ─────────────────────────────────────────────────────────── */

/** Client node: sends/receives chat, does NOT relay CHAT or DATA. */
#ifndef RIVR_ROLE_CLIENT
#  define RIVR_ROLE_CLIENT       0
#endif

/** Repeater node: relays CHAT, DATA, and all control frames. */
#ifndef RIVR_ROLE_REPEATER
#  define RIVR_ROLE_REPEATER     0
#endif

/**
 * Gateway node (stub): reserved for a future IP-bridge implementation.
 * When RIVR_ROLE_GATEWAY=1 the node boots but performs no active relay;
 * the application layer is responsible for bridging RF↔IP.
 * Cannot be combined with CLIENT or REPEATER.
 */
#ifndef RIVR_ROLE_GATEWAY
#  define RIVR_ROLE_GATEWAY      0
#endif

/** Congestion-aware relay suppression (effective only when ROLE_REPEATER=1). */
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER   0
#endif

/* ── Radio chip selection ────────────────────────────────────────────────── */

/** Use SX1262 / EBYTE E22 driver (firmware_core/radio_sx1262.c). */
#ifndef RIVR_RADIO_SX1262
#  define RIVR_RADIO_SX1262      1
#endif

/** Use SX1276 / HopeRF RFM95 driver (firmware_core/radio_sx1276.c). */
#ifndef RIVR_RADIO_SX1276
#  define RIVR_RADIO_SX1276      0
#endif

/* ── Optional feature flags ─────────────────────────────────────────────── */

/**
 * PSK-AES-128-CTR frame-level encryption.
 * When enabled, set RIVR_PSK_HEX to a 32-hex-digit key string.
 * Example: -DRIVR_PSK_HEX=\"0102030405060708090a0b0c0d0e0f10\"
 */
#ifndef RIVR_FEATURE_ENCRYPTION
#  define RIVR_FEATURE_ENCRYPTION   0
#endif

/**
 * Payload compression (reserved for future use — miniz / heatshrink).
 * Setting this to 1 currently has no effect; the infrastructure is
 * in place for a future implementation.
 */
#ifndef RIVR_FEATURE_COMPRESSION
#  define RIVR_FEATURE_COMPRESSION  0
#endif

/**
 * BLE transport bridge for the companion app.
 * When enabled, the ESP-IDF BLE stack is initialised and the NUS-style
 * GATT service is advertised at startup.
 */
#ifndef RIVR_FEATURE_BLE
#  define RIVR_FEATURE_BLE          0
#endif

/* ── Security flags ─────────────────────────────────────────────────────── */

/**
 * Require Ed25519 signature on all PKT_PROG_PUSH OTA payloads.
 * Should be set in production builds; defaults off for dev convenience.
 * Implementation: firmware_core/rivr_ota.c + ed25519_verify.c
 */
#ifndef RIVR_SIGNED_PROG
#  define RIVR_SIGNED_PROG          0
#endif

/**
 * Require HMAC-SHA-256 signature on all @PARAMS OTA parameter updates.
 *
 * When RIVR_FEATURE_SIGNED_PARAMS=1:
 *   - A valid   sig= field causes the update to be accepted.
 *   - A missing sig= field is accepted only when
 *     RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS=1 (dev grace period).
 *   - An invalid sig= field always causes the update to be rejected.
 *
 * When RIVR_FEATURE_SIGNED_PARAMS=0 (default):
 *   - sig= field is silently ignored.
 *   - All @PARAMS are accepted unconditionally (backward compatible).
 *
 * Wire format:
 *   @PARAMS beacon=<ms> chat=<ms> data=<ms> duty=<1..10>
 *           role=<client|repeater|gateway> sig=<64 hex chars>
 *
 * The HMAC covers the exact ASCII bytes BEFORE the " sig=" token.
 * Key source: RIVR_PARAMS_PSK_HEX (compile-time) overridable per-board.
 */
#ifndef RIVR_FEATURE_SIGNED_PARAMS
#  define RIVR_FEATURE_SIGNED_PARAMS        0
#endif

/**
 * When RIVR_FEATURE_SIGNED_PARAMS=1, also accept unsigned @PARAMS.
 * Default ON to ease initial deployment.  Set to 0 for production hardening:
 *   -DRIVR_FEATURE_ALLOW_UNSIGNED_PARAMS=0 -DRIVR_FEATURE_SIGNED_PARAMS=1
 */
#ifndef RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS
#  define RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS  1
#endif

/**
 * Pre-shared key for @PARAMS HMAC-SHA-256 signing (64 hex nibbles = 32 bytes).
 *
 * ⚠ SECURITY: The default all-zero key is insecure and should be replaced
 *   in every production build with a secret value unique per fleet/network:
 *     -DRIVR_PARAMS_PSK_HEX=\"<64 random hex chars>\"
 *
 * Example (generate with: openssl rand -hex 32):
 *   -DRIVR_PARAMS_PSK_HEX=\"a3f2...1b9c\"
 */
#ifndef RIVR_PARAMS_PSK_HEX
#  define RIVR_PARAMS_PSK_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

/* Compile-time guard: if HMAC-signed @PARAMS is enabled, the default all-zero
 * PSK must not be used in a real build.  Override RIVR_PARAMS_PSK_HEX via a
 * board-specific build_flags entry before releasing firmware. */
#if RIVR_FEATURE_SIGNED_PARAMS
_Static_assert(
    __builtin_strcmp(RIVR_PARAMS_PSK_HEX,
        "0000000000000000000000000000000000000000000000000000000000000000") != 0,
    "RIVR_FEATURE_SIGNED_PARAMS is enabled but RIVR_PARAMS_PSK_HEX is still "
    "the insecure all-zero default.  Set a secret key via -DRIVR_PARAMS_PSK_HEX "
    "in your board's build_flags before building release firmware."
);
#endif

/* ── Build mode flags ───────────────────────────────────────────────────── */

/** Simulation / host-test mode — disables hardware peripherals. */
#ifndef RIVR_SIM_MODE
#  define RIVR_SIM_MODE             0
#endif

/* ── Role-specific capacity defaults ────────────────────────────────────── *
 *                                                                            *
 * These constants size fixed BSS structures.  Smaller CLIENT values save     *
 * ~2 kB of DRAM; larger REPEATER/GATEWAY values increase throughput.        *
 * Override per-board in variants/<board>/config.h or via -D flags.          *
 *                                                                            *
 * RIVR_ROUTE_CACHE_SIZE  — capacity of the hybrid unicast route cache.      *
 *   CLIENT   : 32  (end-device, few concurrent peers)                       *
 *   REPEATER : 64  (infrastructure hub, many routes to track)               *
 *   GATEWAY  : 64  (same as repeater — bridges all mesh routes to IP)       *
 *   generic  : 64  (sim/test: full size)                                    *
 *                                                                            *
 * RIVR_RETRY_TABLE_SIZE  — concurrent in-flight unicast ACK-wait slots.     *
 *   CLIENT   :  8  (sends its own traffic only)                              *
 *   REPEATER : 32  (relays many directed frames simultaneously)              *
 *   GATEWAY  : 32  (same as repeater)                                       *
 *   generic  : 16  (sim/test: mid-range)                                    *
 * ─────────────────────────────────────────────────────────────────────── */

#ifndef RIVR_ROUTE_CACHE_SIZE
#  if RIVR_ROLE_CLIENT
#    define RIVR_ROUTE_CACHE_SIZE   32u
#  else
#    define RIVR_ROUTE_CACHE_SIZE   64u  /**< repeater / gateway / generic */
#  endif
#endif

#ifndef RIVR_RETRY_TABLE_SIZE
#  if RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY
#    define RIVR_RETRY_TABLE_SIZE   32u
#  elif RIVR_ROLE_CLIENT
#    define RIVR_RETRY_TABLE_SIZE    8u
#  else
#    define RIVR_RETRY_TABLE_SIZE   16u  /**< sim/test: mid-range */
#  endif
#endif

/* ── Next-gen routing feature flags (Phase 0 baseline — all default OFF) ── *
 *                                                                            *
 * All three flags default to 0 on every role/board.                         *
 * Enable in variants/<board>/config.h when the corresponding phase is       *
 * ready and validated.                                                       *
 *                                                                            *
 * Conservative design: when a flag is 0 the code path is identical to the   *
 * pre-Phase-0 behavior — no CPU, RAM, or airtime overhead.                  *
 * ─────────────────────────────────────────────────────────────────────────── */

/**
 * Replace hop-count-only route selection with ETX + airtime-aware scoring.
 * Phase 2 implementation prerequisite.
 * When 0 (default): existing route_cache_best_hop() behavior unchanged.
 * When 1: composite scoring uses etx_x8 denominator; neighbor table must
 *          carry etx_x8 data (added in Phase 1).
 *
 * Default policy (can be overridden in any variants/<board>/config.h):
 *   REPEATER / GATEWAY  → 1   ETX scoring active on relay hubs.
 *   CLIENT / generic    → 0   Conservative; field-validated before enabling.
 */
#ifndef RIVR_FEATURE_AIRTIME_ROUTING
#  if RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY
#    define RIVR_FEATURE_AIRTIME_ROUTING  1
#  else
#    define RIVR_FEATURE_AIRTIME_ROUTING  0
#  endif
#endif

/**
 * Enable channel-load-adaptive forward rate caps and dynamic TTL clamping.
 * Phase 3 implementation prerequisite.
 * When 0 (default): forward_budget_t caps are static role-based constants.
 * When 1: routing_fwdbudget_adapt() adjusts caps each minute window based on
 *          rivr_fabric_get_score() and dutycycle_remaining_us().
 * Meaningful only when RIVR_FABRIC_REPEATER=1.
 *
 * Default policy (can be overridden in any variants/<board>/config.h):
 *   REPEATER / GATEWAY  → 1   Adaptive throttling active on relay hubs.
 *   CLIENT / generic    → 0   Static caps; field-validated before enabling.
 */
#ifndef RIVR_FEATURE_ADAPTIVE_FLOOD
#  if RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY
#    define RIVR_FEATURE_ADAPTIVE_FLOOD  1
#  else
#    define RIVR_FEATURE_ADAPTIVE_FLOOD  0
#  endif
#endif

/**
 * Enable opportunistic relay cancellation (overheard-forward suppression).
 * Phase 4 implementation prerequisite.
 * When 0 (default): relay frames are never cancelled after queuing.
 * When 1: if a neighbor's relay of (src_id,pkt_id) is heard during our jitter
 *          window, our queued copy is skipped by tx_drain_loop() and the
 *          flood_fwd_cancelled_opport_total counter is incremented.
 *
 * Default policy (can be overridden in any variants/<board>/config.h):
 *   REPEATER / GATEWAY  → 1   Relay suppression active on relay hubs.
 *   CLIENT / generic    → 0   Conservative; field-validated before enabling.
 */
#ifndef RIVR_FEATURE_OPPORTUNISTIC_FWD
/* Enabled for ALL roles:
 *  Repeater/Gateway: Phase 4 + 5 reduce channel collisions on relay hubs.
 *  Client: Phase 4 prevents double-relaying control frames (BEACON,
 *          ROUTE_*, ACK) when a repeater has already forwarded them;
 *          Phase 5 suppresses relay when a better-positioned repeater
 *          exists (viable_count>0 && best_direct_score<threshold).
 *  Edge clients (no viable neighbours) are never suppressed by Phase 5
 *  because fwdset_suppress_relay() returns false when viable_count==0. */
#  define RIVR_FEATURE_OPPORTUNISTIC_FWD  1
#endif

/* ── Compile-time guards ─────────────────────────────────────────────────── */

#if RIVR_RADIO_SX1262 && RIVR_RADIO_SX1276
#  error "RIVR: Both RIVR_RADIO_SX1262 and RIVR_RADIO_SX1276 are set — pick one."
#endif

#if RIVR_ROLE_CLIENT && RIVR_ROLE_REPEATER
#  error "RIVR: Both RIVR_ROLE_CLIENT and RIVR_ROLE_REPEATER are set — pick one."
#endif

#if RIVR_ROLE_GATEWAY && (RIVR_ROLE_CLIENT || RIVR_ROLE_REPEATER)
#  error "RIVR: RIVR_ROLE_GATEWAY cannot be combined with RIVR_ROLE_CLIENT or RIVR_ROLE_REPEATER."
#endif

/* Warn (not error) when no role is set — valid for tests / generic builds */
#if !RIVR_ROLE_CLIENT && !RIVR_ROLE_REPEATER && !RIVR_ROLE_GATEWAY && !RIVR_SIM_MODE
#  pragma message("RIVR: No role flag set (RIVR_ROLE_CLIENT / RIVR_ROLE_REPEATER / RIVR_ROLE_GATEWAY). Using generic defaults.")
#endif

/* ── Feature string (for build_info banner) ─────────────────────────────── */

#if RIVR_FEATURE_ENCRYPTION
#  define _RIVR_FEAT_ENC  "+enc"
#else
#  define _RIVR_FEAT_ENC  ""
#endif

#if RIVR_FEATURE_COMPRESSION
#  define _RIVR_FEAT_COMP "+comp"
#else
#  define _RIVR_FEAT_COMP ""
#endif

#if RIVR_FEATURE_BLE
#  define _RIVR_FEAT_BLE  "+ble"
#else
#  define _RIVR_FEAT_BLE  ""
#endif

#if RIVR_FEATURE_AIRTIME_ROUTING
#  define _RIVR_FEAT_AIR  "+air"
#else
#  define _RIVR_FEAT_AIR  ""
#endif

#if RIVR_FEATURE_ADAPTIVE_FLOOD
#  define _RIVR_FEAT_AFLOOD "+aflood"
#else
#  define _RIVR_FEAT_AFLOOD ""
#endif

#if RIVR_FEATURE_OPPORTUNISTIC_FWD
#  define _RIVR_FEAT_OPFWD "+opfwd"
#else
#  define _RIVR_FEAT_OPFWD ""
#endif

/** Concatenated feature tag string, e.g. "+enc+ble" — for build_info banner. */
#define RIVR_FEATURE_TAG_STR  (_RIVR_FEAT_ENC _RIVR_FEAT_COMP _RIVR_FEAT_BLE \
                               _RIVR_FEAT_AIR _RIVR_FEAT_AFLOOD _RIVR_FEAT_OPFWD)
