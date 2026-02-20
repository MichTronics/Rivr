/**
 * @file  ringbuf.h
 * @brief Lock-free SPSC (Single-Producer Single-Consumer) ring buffer.
 *
 * DESIGN RULES (determinism + bounded memory)
 * ─────────────────────────────────────────────
 *  • Fixed capacity: `CAP` elements, `sizeof(T)` each. No heap allocation.
 *  • Exactly ONE writer (ISR / producer task) and ONE reader (main loop).
 *  • Uses sequential-consistency atomic loads/stores; barriers ensure
 *    memory ordering without a mutex.
 *  • `rb_try_push` drops the frame silently and increments `drops` counter
 *    when the buffer is full (back-pressure via visible loss accounting).
 *  • Never call any ringbuf function from both ISR AND task for the same
 *    direction; ISR only writes to RX buf, main loop only writes to TX buf.
 *
 * USAGE
 * ─────
 *  // Define once (in a .c file or as a global):
 *  RINGBUF_DEFINE(rf_rx_buf, uint8_t[256], 8);
 *
 *  // Producer (ISR):
 *  rb_try_push(&rf_rx_buf, frame);
 *
 *  // Consumer (main loop):
 *  uint8_t frame[256];
 *  if (rb_pop(&rf_rx_buf, frame)) { ... }
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

/* ─────────────────────────────── generic ring buffer ─────────────────────── */

/**
 * @brief Ring-buffer descriptor (type-erased).
 *
 * Do not use directly — use RINGBUF_DEFINE() and the typed accessors below.
 */
typedef struct {
    uint8_t        *buf;          /**< Storage array: CAP * item_size bytes   */
    uint32_t        cap;          /**< Capacity in items (must be power of 2) */
    uint32_t        item_size;    /**< Bytes per item                         */
    atomic_uint_fast32_t head;    /**< Producer index (written by ISR)        */
    atomic_uint_fast32_t tail;    /**< Consumer index (written by main loop)  */
    atomic_uint_fast32_t drops;   /**< Items dropped due to full buffer       */
} rb_t;

/* ─── initialiser ─── */
static inline void rb_init(rb_t *rb, void *storage, uint32_t cap, uint32_t item_size)
{
    rb->buf       = (uint8_t *)storage;
    rb->cap       = cap;
    rb->item_size = item_size;
    atomic_store_explicit(&rb->head,  0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail,  0, memory_order_relaxed);
    atomic_store_explicit(&rb->drops, 0, memory_order_relaxed);
}

/* ─── push (producer / ISR side) ─── */
static inline bool rb_try_push(rb_t *rb, const void *item)
{
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t next = (head + 1u) & (rb->cap - 1u);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (next == tail) {
        /* Buffer full – count the drop and return false */
        atomic_fetch_add_explicit(&rb->drops, 1u, memory_order_relaxed);
        return false;
    }
    memcpy(rb->buf + head * rb->item_size, item, rb->item_size);
    atomic_store_explicit(&rb->head, next, memory_order_release);
    return true;
}

/* ─── pop (consumer / main-loop side) ─── */
static inline bool rb_pop(rb_t *rb, void *out)
{
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (tail == head) return false;   /* empty */

    memcpy(out, rb->buf + tail * rb->item_size, rb->item_size);
    uint32_t next = (tail + 1u) & (rb->cap - 1u);
    atomic_store_explicit(&rb->tail, next, memory_order_release);
    return true;
}

/* Snapshot of drop count (safe to call from any context) */
static inline uint32_t rb_drops(const rb_t *rb)
{
    return atomic_load_explicit(&rb->drops, memory_order_relaxed);
}

/* Number of items currently available to read */
static inline uint32_t rb_available(const rb_t *rb)
{
    uint32_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    return (h - t) & (rb->cap - 1u);
}

/* ─────────────────────── typed convenience macros ─────────────────────────── */

/**
 * RINGBUF_DEFINE(name, ItemType, cap)
 *
 * Declares a statically allocated ring-buffer for items of ItemType.
 * cap MUST be a power of 2.
 *
 * Example:
 *   RINGBUF_DEFINE(rf_rx_buf, rf_frame_t, 8);
 */
#define RINGBUF_DEFINE(name, ItemType, cap)             \
    static ItemType  _##name##_storage[(cap)];          \
    static rb_t name;                                   \
    /* Call rb_init(&name, _##name##_storage, (cap), sizeof(ItemType)) at boot */

/**
 * Helper to initialise a ring-buffer declared with RINGBUF_DEFINE.
 * Must be called once from non-ISR context before any push/pop.
 */
#define RINGBUF_INIT(name, ItemType, cap) \
    rb_init(&(name), _##name##_storage, (cap), sizeof(ItemType))

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */
