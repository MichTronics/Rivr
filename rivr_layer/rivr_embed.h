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

/* ── rivr_core FFI types (mirrors Rust structs) ─────────────────────────── */

/**
 * C mirror of Rust `Stamp { clock: u8, tick: u64 }`.
 * Matches the repr(C) layout exported by rivr_core/src/ffi.rs.
 */
typedef struct {
    uint8_t  clock;
    uint64_t tick;
} rivr_stamp_t;

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
 * stores the result in the static engine slot.
 *
 * @param program_src  null-terminated RIVR source (stored in flash / ROM)
 * @return 0 on success, negative errno on parse/compile failure
 */
int32_t rivr_engine_init(const char *program_src);

/**
 * @brief Inject a single event into the named source queue.
 *
 * Thread context: main loop ONLY (not ISR).
 *
 * @param source_name  null-terminated source name (e.g. "rf_rx\0")
 * @param event        pointer to populated rivr_event_t
 * @return 0 on success, -1 if source not found, -2 if queue full
 */
int32_t rivr_inject_event(const char *source_name, const rivr_event_t *event);

/**
 * @brief Run the engine for up to `max_steps` scheduler cycles.
 *
 * Processes pending events and fires registered emit callbacks.
 * Returns the number of events actually processed.
 *
 * @param max_steps  upper bound on scheduler iterations (prevents starvation)
 * @return           actual steps taken
 */
uint32_t rivr_engine_run(uint32_t max_steps);

/** Returns the engine's last-known monotonic clock value. */
uint64_t rivr_engine_clock_now(uint8_t clock_id);

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
