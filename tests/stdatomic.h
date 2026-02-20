/*
 * tests/stdatomic.h  –  single-threaded host shim for <stdatomic.h>
 *
 * Used ONLY when building the acceptance-test binary with MSVC.
 * gcc keeps -Itests off its include path, so the real <stdatomic.h>
 * is still used for GCC builds.
 *
 * The host test binary is always single-threaded, so "relaxed"
 * plain loads/stores are semantically correct.
 */
#pragma once
#include <stdint.h>

/* ---- types --------------------------------------------------------- */
typedef uint32_t  atomic_uint_fast32_t;
typedef uint32_t  atomic_uint;
typedef int32_t   atomic_int;

/* ---- init ---------------------------------------------------------- */
#define ATOMIC_VAR_INIT(x)   (x)

/* ---- memory-order enum (values don't matter for single-thread) ----- */
typedef enum memory_order {
    memory_order_relaxed = 0,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

/* ---- helpers (avoid multiple-evaluation of macro args) ------------ */
static __inline uint32_t _aload32(const uint32_t *p)
    { return *p; }
static __inline void _astore32(uint32_t *p, uint32_t v)
    { *p = v; }
static __inline uint32_t _afetch_add32(uint32_t *p, uint32_t v)
    { uint32_t old = *p; *p += v; return old; }
static __inline int _acas32(uint32_t *obj, uint32_t *expected, uint32_t desired,
                             memory_order succ, memory_order fail)
{
    (void)succ; (void)fail;
    if (*obj == *expected) { *obj = desired; return 1; }
    *expected = *obj;       return 0;
}

/* ---- public macros ------------------------------------------------- */
#define atomic_load(p)                              _aload32((p))
#define atomic_store(p, v)                          _astore32((p),(v))
#define atomic_fetch_add(p, v)                      _afetch_add32((p),(v))
#define atomic_load_explicit(p, mo)                 _aload32((p))
#define atomic_store_explicit(p, v, mo)             _astore32((p),(v))
#define atomic_fetch_add_explicit(p, v, mo)         _afetch_add32((p),(v))

#define atomic_compare_exchange_weak_explicit(obj,exp,des,succ,fail) \
    _acas32((obj),(exp),(des),(succ),(fail))
#define atomic_compare_exchange_strong_explicit(obj,exp,des,succ,fail) \
    _acas32((obj),(exp),(des),(succ),(fail))
#define atomic_compare_exchange_weak(obj,exp,des) \
    _acas32((obj),(exp),(des),memory_order_seq_cst,memory_order_seq_cst)
#define atomic_compare_exchange_strong(obj,exp,des) \
    _acas32((obj),(exp),(des),memory_order_seq_cst,memory_order_seq_cst)
