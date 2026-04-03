/**
 * compat/stdatomic.h — C11 atomics shim for arm-none-eabi C++ mode
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus

#ifndef memory_order_relaxed
#  define memory_order_relaxed __ATOMIC_RELAXED
#  define memory_order_acquire __ATOMIC_ACQUIRE
#  define memory_order_release __ATOMIC_RELEASE
#  define memory_order_acq_rel __ATOMIC_ACQ_REL
#  define memory_order_seq_cst __ATOMIC_SEQ_CST
#endif

typedef volatile bool atomic_bool;
typedef volatile int atomic_int;
typedef volatile unsigned int atomic_uint;
typedef volatile uint_fast32_t atomic_uint_fast32_t;
typedef volatile int_fast32_t atomic_int_fast32_t;
typedef volatile uint32_t atomic_uint32_t;

#ifndef ATOMIC_VAR_INIT
#  define ATOMIC_VAR_INIT(v) (v)
#endif

#define atomic_store(obj, val) __atomic_store_n((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_load(obj) __atomic_load_n((obj), __ATOMIC_SEQ_CST)
#define atomic_store_explicit(obj, val, order) __atomic_store_n((obj), (val), (order))
#define atomic_load_explicit(obj, order) __atomic_load_n((obj), (order))
#define atomic_fetch_add_explicit(obj, val, order) __atomic_fetch_add((obj), (val), (order))
#define atomic_fetch_sub_explicit(obj, val, order) __atomic_fetch_sub((obj), (val), (order))
#define atomic_fetch_or_explicit(obj, val, order) __atomic_fetch_or((obj), (val), (order))
#define atomic_fetch_and_explicit(obj, val, order) __atomic_fetch_and((obj), (val), (order))
#define atomic_exchange_explicit(obj, val, order) __atomic_exchange_n((obj), (val), (order))
#define atomic_compare_exchange_strong_explicit(obj, exp, val, succ, fail) \
    __atomic_compare_exchange_n((obj), (exp), (val), 0, (succ), (fail))
#define atomic_compare_exchange_weak_explicit(obj, exp, val, succ, fail) \
    __atomic_compare_exchange_n((obj), (exp), (val), 1, (succ), (fail))
#define atomic_thread_fence(order) __atomic_thread_fence(order)

#else

#include_next <stdatomic.h>

#endif
