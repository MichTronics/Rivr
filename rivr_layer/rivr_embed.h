/**
 * @file  rivr_embed.h
 * @brief Glue layer between the C firmware core and the rivr_core Rust library.
 *
 * ARCHITECTURE
 * ────────────
 *  rivr_core is compiled as a `staticlib` Rust crate and linked into the
 *  ESP32 firmware image.  This header declares the C-visible FFI surface
 *  exported from rivr_core via `#[no_mangle] extern "C"` functions.
 *
 *  The rivr_embed module owns ONE engine instance (g_rivr_engine) and
 *  implements the main-loop integration function rivr_tick().
 *
 * MEMORY MODEL
 * ────────────
 *  Engine state lives in static RAM (no heap).  The Rust crate is compiled
 *  with the `no_std` profile (no alloc feature), so all values use
 *  FixedText<128> / FixedBytes<256> stack types.
 *
 * CALL GRAPH (main loop)
 * ──────────────────────
 *   rivr_tick()
 *     ├─ rivr_sources_drain()       ← pull frames from ringbuf, inject events
 *     ├─ rivr_engine_run(steps)     ← FFI call into rivr_core
 *     └─ (emit callbacks registered at init fire when RIVR emits)
 *           └─ rf_tx_sink_cb() / log_sink_cb()
 */

#ifndef RIVR_EMBED_H
#define RIVR_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "../firmware_core/timebase.h"
/* ── Result type returned by all rivr_core FFI calls ──────────────────────
 *
 * C mirror of Rust `RivrResult`.  Use `rc.code != RIVR_OK` as the error check.
 * Never test the raw code value with -1 / -2 etc. -- use the constants below.
 */
typedef struct {
    uint32_t code;           /**< 0 = success.  Non-zero = RIVR_ERR_* code. */
    uint32_t cycles_used;    /**< Scheduler steps consumed (meaningful for rivr_engine_run). */
    uint32_t gas_remaining;  /**< Remaining budget; 0 = gas exhausted (starvation risk). */
} rivr_result_t;

/* Error-code constants (match Rust RIVR_ERR_* in ffi.rs). */
#define RIVR_OK               0u
#define RIVR_ERR_NULL_PTR     1u  /**< Null pointer argument. */
#define RIVR_ERR_UTF8         2u  /**< Non-UTF-8 C string. */
#define RIVR_ERR_PARSE        3u  /**< RIVR parse error. */
#define RIVR_ERR_COMPILE      4u  /**< RIVR compile error. */
#define RIVR_ERR_NOT_INIT     5u  /**< Engine not yet initialised. */
#define RIVR_ERR_SRC_UNKNOWN  6u  /**< Source name not found in graph. */
#define RIVR_ERR_NODE_LIMIT   7u  /**< Compiled graph exceeds RIVR_MAX_NODES (64). */
/* ── rivr_core FFI types (mirrors Rust structs) ─────────────────────────── */

/**
 * C mirror of Rust `Stamp { clock: u8, tick: u64 }`.
 * Matches the repr(C) layout exported by rivr_core/src/ffi.rs.
 * NOTE: rivr_stamp_t is defined in firmware_core/timebase.h and re-used here.
 */
/* rivr_stamp_t is typedef'd in timebase.h (included above) */

/** Value tag discriminants (mirrors Rust Value enum). */
typedef enum {
    RIVR_VAL_UNIT  = 0,
    RIVR_VAL_INT   = 1,
    RIVR_VAL_BOOL  = 2,
    RIVR_VAL_STR   = 3,
    RIVR_VAL_BYTES = 4,
} rivr_val_tag_t;

/** Fixed-length string value (no heap). */
typedef struct {
    uint8_t buf[128];
    uint8_t len;
} rivr_fixed_str_t;

/** Fixed-length byte value (no heap). */
typedef struct {
    uint8_t buf[256];
    uint16_t len;
} rivr_fixed_bytes_t;

/** C mirror of the no-alloc `Value` union. */
typedef struct {
    rivr_val_tag_t tag;
    union {
        int64_t            as_int;
        bool               as_bool;
        rivr_fixed_str_t   as_str;
        rivr_fixed_bytes_t as_bytes;
    };
} rivr_value_t;

/** C mirror of `Event { stamp, v, tag, seq }`. */
typedef struct {
    rivr_stamp_t  stamp;
    rivr_value_t  v;
    uint8_t       kind_tag[32];  /**< Optional kind tag (e.g. "CHAT\0")       */
    uint32_t      seq;
} rivr_event_t;

/* ── Engine FFI declarations (implemented in rivr_core/src/ffi.rs) ──────── */

/**
 * @brief Initialise a fresh engine from a compiled RIVR program string.
 *
 * Parses and compiles `program_src`, wires up the sink callbacks, and
 * stores the result in the static engine slot.  Rejects programs whose node
 * count exceeds RIVR_MAX_NODES (64).
 *
 * @param program_src  null-terminated RIVR source (stored in flash / ROM)
 * @return rivr_result_t  .code == RIVR_OK on success, RIVR_ERR_* on failure
 */
rivr_result_t rivr_engine_init(const char *program_src);

/**
 * @brief Inject a single event into the named source queue.
 *
 * Thread context: main loop ONLY (not ISR).
 *
 * @param source_name  null-terminated source name (e.g. "rf_rx\0")
 * @param event        pointer to populated rivr_event_t
 * @return rivr_result_t  .code == RIVR_OK, or RIVR_ERR_SRC_UNKNOWN / RIVR_ERR_NULL_PTR
 */
rivr_result_t rivr_inject_event(const char *source_name, const rivr_event_t *event);

/**
 * @brief Run the engine for up to `max_steps` scheduler cycles.
 *
 * Calls the registered watchdog hook every 64 steps so the hardware watchdog
 * does not trigger on large programs.  Returns a `rivr_result_t` with:
 *   .cycles_used   – actual steps taken
 *   .gas_remaining – 0 if the scheduler was still active at return (starvation)
 *
 * @param max_steps  upper bound on scheduler iterations (prevents starvation)
 * @return           rivr_result_t (code is always RIVR_OK if engine is ready)
 */
rivr_result_t rivr_engine_run(uint32_t max_steps);

/** Returns the engine's last-known monotonic clock value. */
uint64_t rivr_engine_clock_now(uint8_t clock_id);

/** Returns the number of nodes in the compiled stream graph (diagnostic). */
uint32_t rivr_engine_node_count(void);

/**
 * @brief Register an optional watchdog-reset callback.
 *
 * When set, the callback is invoked every 64 scheduler steps inside
 * `rivr_engine_run()`.  Typical use: pass `esp_task_wdt_reset`.
 * Pass `NULL` to disable.
 */
typedef void (*rivr_watchdog_hook_t)(void);
void rivr_set_watchdog_hook(rivr_watchdog_hook_t hook);

/**
 * @brief Register the C emit-dispatch callback with the Rust FFI layer.
 *
 * MUST be called once before rivr_engine_init().  The callback is invoked
 * by the Rust engine every time RIVR executes an `emit { ... }` statement.
 *
 * @param f  pointer to rivr_emit_dispatch() (declared below)
 */
typedef void (*rivr_dispatch_fn_t)(const char *sink_name, const rivr_value_t *v);
void rivr_set_emit_dispatch(rivr_dispatch_fn_t f);

/* ── Sink callback registration (call before rivr_engine_init) ──────────── */

/** Prototype for emit sink callbacks. */
typedef void (*rivr_sink_cb_t)(const rivr_value_t *v, void *user_ctx);

/**
 * @brief Register a callback for a named emit sink.
 *
 * The callback will be invoked (from within rivr_engine_run) each time
 * RIVR emits a value on the named sink.
 *
 * @param sink_name  name of the emit target (e.g. "rf_tx", "log")
 * @param cb         function pointer
 * @param user_ctx   opaque context passed back to cb
 */
void rivr_register_sink(const char *sink_name, rivr_sink_cb_t cb, void *user_ctx);

/* ── Main integration point ──────────────────────────────────────────────── */

/**
 * @brief Single RIVR processing tick – call from main loop every iteration.
 *
 * 1. Pulls up to RX_DRAIN_LIMIT frames from rf_rx_ringbuf.
 * 2. Converts each frame to a rivr_event_t and injects into the engine.
 * 3. Runs the engine for up to MAX_ENGINE_STEPS scheduler cycles.
 * 4. Returns total events processed this tick.
 */
uint32_t rivr_tick(void);

/* ── Initialisation ──────────────────────────────────────────────────────── */

/**
 * @brief Initialise the RIVR embed layer.
 *
 * Registers sink callbacks, then calls rivr_engine_init with the default
 * compiled-in program.
 */
void rivr_embed_init(void);

/** Print engine diagnostics to the UART log channel. */
void rivr_embed_print_stats(void);

/* ── Shared routing state (owned by rivr_embed.c) ───────────────────────── *
 *
 * Exposed here so both rivr_sources.c (RX path) and rivr_sinks.c (TX path)
 * can access the route cache and node identity without circular deps.
 * ─────────────────────────────────────────────────────────────────────────── */
#include "../firmware_core/route_cache.h"
#include "../firmware_core/pending_queue.h"

/** Module-global route cache — initialised in rivr_embed_init(). */
extern route_cache_t    g_route_cache;

/** Phase-D pending-message queue — initialised in rivr_embed_init(). */
extern pending_queue_t  g_pending_queue;

/**
 * This node's unique 32-bit ID.
 * In production: derive from ESP32 MAC (e.g. esp_efuse_mac_get_default()).
 * In simulation: set by sim_inject_packets() caller before rivr_embed_init().
 */
extern uint32_t g_my_node_id;

/**
 * Monotonically increasing sequence counter for control packets
 * (ROUTE_REQ, ROUTE_RPL) generated by this node.
 */
extern uint32_t g_ctrl_seq;

#ifdef __cplusplus
}
#endif

#endif /* RIVR_EMBED_H */
