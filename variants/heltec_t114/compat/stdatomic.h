/**
 * compat/stdatomic.h — C11 atomics shim for arm-none-eabi C++ mode
 *
 * PROBLEM
 * -------
 * arm-none-eabi-gcc's <stdatomic.h> implements atomic_*_explicit() with
 * __auto_type (a GCC C extension).  When ringbuf.h is included from a .cpp
 * translation unit, the compiler may reject __auto_type with:
 *   "error: '__auto_type' was not declared in this scope"
 *
 * FIX
 * ---
 * In C++ mode: typedef atomic_uint_fast32_t as plain volatile and replace
 * the type-generic macros with GCC __atomic_* built-ins, which work in both
 * C and C++ regardless of language standard.  Struct layout is identical on
 * ARM (volatile uint32_t == _Atomic uint32_t == 4 bytes, 4-byte aligned).
 *
 * In C mode: fall through to the real system <stdatomic.h> via #include_next
 * so standard C11 behaviour is preserved.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus

/* ── Memory order constants ─────────────────────────────────────────────── */
#ifndef memory_order_relaxed
#  define memory_order_relaxed  __ATOMIC_RELAXED
#  define memory_order_acquire  __ATOMIC_ACQUIRE
#  define memory_order_release  __ATOMIC_RELEASE
#  define memory_order_acq_rel  __ATOMIC_ACQ_REL
#  define memory_order_seq_cst  __ATOMIC_SEQ_CST
#endif

/* ── Atomic type aliases (same width/alignment as _Atomic on ARM) ────────── */
typedef volatile bool           atomic_bool;
typedef volatile int            atomic_int;
typedef volatile unsigned int   atomic_uint;
typedef volatile uint_fast32_t  atomic_uint_fast32_t;
typedef volatile int_fast32_t   atomic_int_fast32_t;
typedef volatile uint32_t       atomic_uint32_t;

/* ── Initializers ─────────────────────────────────────────────────────────── */
#define ATOMIC_VAR_INIT(v)  (v)

/* ── Generic atomic operations via GCC __atomic_* built-ins ──────────────── */
#define atomic_store(obj, val)                     __atomic_store_n((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_load(obj)                            __atomic_load_n((obj), __ATOMIC_SEQ_CST)
#define atomic_store_explicit(obj, val, order)     __atomic_store_n((obj), (val), (order))
#define atomic_load_explicit(obj, order)            __atomic_load_n((obj), (order))
#define atomic_fetch_add_explicit(obj, val, order) __atomic_fetch_add((obj), (val), (order))
#define atomic_fetch_sub_explicit(obj, val, order) __atomic_fetch_sub((obj), (val), (order))
#define atomic_fetch_or_explicit(obj, val, order)  __atomic_fetch_or((obj), (val), (order))
#define atomic_fetch_and_explicit(obj, val, order) __atomic_fetch_and((obj), (val), (order))
#define atomic_exchange_explicit(obj, val, order)  __atomic_exchange_n((obj), (val), (order))
#define atomic_compare_exchange_strong_explicit(obj, exp, val, succ, fail) \
    __atomic_compare_exchange_n((obj), (exp), (val), 0, (succ), (fail))
#define atomic_compare_exchange_weak_explicit(obj, exp, val, succ, fail) \
    __atomic_compare_exchange_n((obj), (exp), (val), 1, (succ), (fail))
#define atomic_thread_fence(order)                 __atomic_thread_fence(order)

#else  /* C mode */

/* Fall through to the real <stdatomic.h> from the toolchain.
 * #include_next skips the current compat/ directory and picks up the next
 * <stdatomic.h> in the search path (i.e. the one from arm-none-eabi-gcc). */
#include_next <stdatomic.h>

#endif /* __cplusplus */
