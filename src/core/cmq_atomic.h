#ifndef CMQ_ATOMIC_H
#define CMQ_ATOMIC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "cmq_platform.h"
#include "cmq_types.h"

typedef _Atomic(cmq_u64_t) cmq_atomic_u64;
typedef _Atomic(cmq_u32_t) cmq_atomic_u32;
typedef _Atomic(int)        cmq_atomic_int;
typedef _Atomic(void *)     cmq_atomic_ptr;

#define CMQ_ATOMIC_RELAXED memory_order_relaxed
#define CMQ_ATOMIC_ACQUIRE memory_order_acquire
#define CMQ_ATOMIC_RELEASE memory_order_release
#define CMQ_ATOMIC_ACQ_REL memory_order_acq_rel
#define CMQ_ATOMIC_SEQ_CST memory_order_seq_cst

static cmq_inline cmq_u64_t cmq_atomic_load_u64(cmq_atomic_u64 *a, memory_order mo) {
    return atomic_load_explicit(a, mo);
}

static cmq_inline void cmq_atomic_store_u64(cmq_atomic_u64 *a, cmq_u64_t v, memory_order mo) {
    atomic_store_explicit(a, v, mo);
}

static cmq_inline cmq_u64_t cmq_atomic_fetch_add_u64(cmq_atomic_u64 *a, cmq_u64_t v, memory_order mo) {
    return atomic_fetch_add_explicit(a, v, mo);
}

static cmq_inline cmq_u32_t cmq_atomic_load_u32(cmq_atomic_u32 *a, memory_order mo) {
    return atomic_load_explicit(a, mo);
}

static cmq_inline void cmq_atomic_store_u32(cmq_atomic_u32 *a, cmq_u32_t v, memory_order mo) {
    atomic_store_explicit(a, v, mo);
}

static cmq_inline cmq_u32_t cmq_atomic_fetch_add_u32(cmq_atomic_u32 *a, cmq_u32_t v, memory_order mo) {
    return atomic_fetch_add_explicit(a, v, mo);
}

static cmq_inline int cmq_atomic_load_int(cmq_atomic_int *a, memory_order mo) {
    return atomic_load_explicit(a, mo);
}

static cmq_inline void cmq_atomic_store_int(cmq_atomic_int *a, int v, memory_order mo) {
    atomic_store_explicit(a, v, mo);
}

static cmq_inline bool cmq_atomic_compare_exchange_ptr(cmq_atomic_ptr *a, void **expected, void *desired, memory_order mo) {
    memory_order fail_mo = (mo == memory_order_release) ? memory_order_relaxed : mo;
    if (fail_mo == memory_order_acq_rel) fail_mo = memory_order_acquire;
    return atomic_compare_exchange_strong_explicit(a, expected, desired, mo, fail_mo);
}

static cmq_inline void *cmq_atomic_load_ptr(cmq_atomic_ptr *a, memory_order mo) {
    return atomic_load_explicit(a, mo);
}

static cmq_inline void cmq_atomic_store_ptr(cmq_atomic_ptr *a, void *v, memory_order mo) {
    atomic_store_explicit(a, v, mo);
}

static cmq_inline bool cmq_atomic_cas_u32(cmq_atomic_u32 *a, cmq_u32_t *expected, cmq_u32_t desired, memory_order mo) {
    memory_order fail_mo = (mo == memory_order_release) ? memory_order_relaxed : mo;
    if (fail_mo == memory_order_acq_rel) fail_mo = memory_order_acquire;
    return atomic_compare_exchange_strong_explicit(a, expected, desired, mo, fail_mo);
}

static cmq_inline bool cmq_atomic_cas_int(cmq_atomic_int *a, int *expected, int desired, memory_order mo) {
    memory_order fail_mo = (mo == memory_order_release) ? memory_order_relaxed : mo;
    if (fail_mo == memory_order_acq_rel) fail_mo = memory_order_acquire;
    return atomic_compare_exchange_strong_explicit(a, expected, desired, mo, fail_mo);
}

static cmq_inline void cmq_atomic_fence(memory_order mo) {
    atomic_thread_fence(mo);
}

static cmq_inline void cmq_atomic_pause(void) {
#if CMQ_ARCH_X86_64 || CMQ_ARCH_X86
    __builtin_ia32_pause();
#elif CMQ_ARCH_AARCH64 || CMQ_ARCH_ARM
    __asm__ __volatile__("yield" ::: "memory");
#else
    (void)0;
#endif
}

#endif
