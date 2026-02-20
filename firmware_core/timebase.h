/**
 * @file  timebase.h
 * @brief Monotonic millisecond / logical-tick timebase.
 *
 * Clock 0 – mono millis (hardware timer, 32-bit → wraps after ~49 days)
 * Clock 1 – Lamport logical tick (incremented on every received mesh packet)
 *
 * Both are safe to READ from any context (atomic reads).
 * WRITE (increment) is done only from main loop / radio ISR respectively.
 */

#ifndef TIMEBASE_H
#define TIMEBASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdatomic.h>

/* ── Shared counters ────────────────────────────────────────────────────── */
extern atomic_uint_fast32_t g_mono_ms;     /**< Clock 0: millis since boot   */
extern atomic_uint_fast32_t g_lmp_tick;    /**< Clock 1: Lamport logical tick */

/* ── Initialisation (call once from app_main before starting tasks) ─────── */
void timebase_init(void);

/* ── Clock 0: monotonic millis ── */

/** Returns current monotonic millisecond counter (safe from any context). */
static inline uint32_t tb_millis(void)
{
    return (uint32_t)atomic_load_explicit(&g_mono_ms, memory_order_relaxed);
}

/** Called from the FreeRTOS tick-hook (every 1 ms) to advance Clock 0. */
void timebase_tick_hook(void);

/* ── Clock 1: Lamport logical tick ── */

/**
 * @brief Advance the Lamport tick.
 *
 * Called by the main loop each time a new mesh packet is accepted.
 * The value returned is the tick ASSIGNED to that packet.
 *
 * Rule: tick_assigned = max(local_tick, sender_tick) + 1
 */
uint32_t tb_lmp_advance(uint32_t sender_tick);

/** Returns current Lamport tick without advancing it. */
static inline uint32_t tb_lmp_now(void)
{
    return (uint32_t)atomic_load_explicit(&g_lmp_tick, memory_order_relaxed);
}

/* ── RIVR Stamp helpers ── */

/**
 * @brief A Stamp value as used by rivr_core (mirrors Rust Stamp { clock, tick }).
 *
 * clock 0 = mono millis,  clock 1 = Lamport tick
 */
typedef struct {
    uint8_t  clock;
    uint64_t tick;
} rivr_stamp_t;

static inline rivr_stamp_t tb_stamp_mono(void)
{
    rivr_stamp_t s = { .clock = 0, .tick = tb_millis() };
    return s;
}

static inline rivr_stamp_t tb_stamp_lmp(uint32_t assigned_tick)
{
    rivr_stamp_t s = { .clock = 1, .tick = assigned_tick };
    return s;
}

#ifdef __cplusplus
}
#endif

#endif /* TIMEBASE_H */
