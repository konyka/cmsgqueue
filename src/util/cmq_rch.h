/* P1: refcounted handle for hot-reloadable server objects (acl, blocklist).
 * See v0.5.1.bundle.md B6. Reader: acquire/release; reload: build new,
 * swap, release old. */

#ifndef CMQ_RCH_H
#define CMQ_RCH_H

#include "cmq_atomic.h"
#include <stdlib.h>

typedef void (*cmq_rch_free_fn)(void *obj);

typedef struct cmq_rch {
    void *ptr;
    cmq_rch_free_fn free_fn;
    cmq_atomic_int refcount;
} cmq_rch_t;

static inline cmq_rch_t *cmq_rch_new(void *ptr, cmq_rch_free_fn free_fn) {
    cmq_rch_t *h = (cmq_rch_t *)malloc(sizeof(cmq_rch_t));
    if (!h) return NULL;
    h->ptr = ptr;
    h->free_fn = free_fn;
    cmq_atomic_store_int(&h->refcount, 1, CMQ_ATOMIC_RELAXED);
    return h;
}

static inline void *cmq_rch_acquire(cmq_rch_t *h) {
    if (!h) return NULL;
    int old;
    do {
        old = cmq_atomic_load_int(&h->refcount, CMQ_ATOMIC_RELAXED);
        if (old <= 0) return NULL;
    } while (!cmq_atomic_cas_int(
                &h->refcount, &old, old + 1,
                CMQ_ATOMIC_ACQ_REL));
    return h->ptr;
}

static inline void cmq_rch_release(cmq_rch_t *h, void *obj) {
    if (!h) return;
    int prev = cmq_atomic_fetch_sub_int(&h->refcount, 1,
                                          CMQ_ATOMIC_ACQ_REL);
    if (prev == 1) {
        if (obj && h->free_fn) h->free_fn(obj);
        free(h);
    }
}

/* Release the OWNER's initial reference (no obj passed — the caller
 * is the owner). Frees the object when refcount hits zero. */
static inline void cmq_rch_release_owner(cmq_rch_t *h) {
    if (!h) return;
    int prev = cmq_atomic_fetch_sub_int(&h->refcount, 1,
                                          CMQ_ATOMIC_ACQ_REL);
    if (prev == 1) {
        if (h->ptr && h->free_fn) h->free_fn(h->ptr);
        free(h);
    }
}

static inline cmq_rch_t *cmq_rch_swap(cmq_rch_t **slot, cmq_rch_t *new_h) {
    void *old = new_h;
    cmq_atomic_compare_exchange_ptr((cmq_atomic_ptr *)slot, &old, new_h,
                                     CMQ_ATOMIC_ACQ_REL);
    return (cmq_rch_t *)old;
}

#endif