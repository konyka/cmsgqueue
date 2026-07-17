#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cmq_mpool.h"
#include "cmq_platform.h"

/* Internal block structure */
typedef struct cmq_mpool_block {
    uint8_t *mem;
    size_t size;      /* total size of this block */
    size_t offset;    /* next free offset within this block */
    struct cmq_mpool_block *next;
} cmq_mpool_block_t;

struct cmq_mpool {
    cmq_mpool_block_t *head;
    cmq_mpool_block_t *tail;
    size_t default_block_size;
};

/* helpers — fail closed on size wrap */
static inline int cmq_align16_ok(size_t v, size_t *out) {
    if (v > SIZE_MAX - 15) return -1;
    *out = (v + 15) & ~((size_t)15);
    return 0;
}

static cmq_mpool_block_t *cmq_mpool_block_create(size_t block_size) {
    if (block_size == 0) return NULL;
    cmq_mpool_block_t *b = (cmq_mpool_block_t *)malloc(sizeof(*b));
    if (!b) return NULL;
    b->size = block_size;
    b->mem = (uint8_t *)malloc(block_size);
    if (!b->mem) {
        free(b);
        return NULL;
    }
    b->offset = 0;
    b->next = NULL;
    return b;
}

cmq_mpool_t *cmq_mpool_create(size_t block_size) {
    if (block_size == 0) block_size = 4096;
    cmq_mpool_t *p = (cmq_mpool_t *)malloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->default_block_size = block_size;
    cmq_mpool_block_t *b = cmq_mpool_block_create(block_size);
    if (!b) {
        free(p);
        return NULL;
    }
    p->head = b;
    p->tail = b;
    return p;
}

void cmq_mpool_destroy(cmq_mpool_t *pool) {
    if (!pool) return;
    cmq_mpool_block_t *cur = pool->head;
    while (cur) {
        cmq_mpool_block_t *n = cur->next;
        free(cur->mem);
        free(cur);
        cur = n;
    }
    free(pool);
}

void *cmq_mpool_alloc(cmq_mpool_t *pool, size_t size) {
    if (!pool) return NULL;
    /* Zero-size: still advance so consecutive calls do not alias. */
    if (size == 0) size = 1;

    cmq_mpool_block_t *b = pool->tail;
    if (!b) return NULL;
    if (b->offset > b->size) return NULL;

    uintptr_t base = (uintptr_t)b->mem + b->offset;
    size_t align_offset = (16 - (base % 16)) % 16;
    if (align_offset > SIZE_MAX - size) return NULL;
    size_t needed = align_offset + size;
    size_t avail = b->size - b->offset;
    if (needed <= avail) {
        b->offset += align_offset;
        void *ptr = b->mem + b->offset;
        b->offset += size;
        return ptr;
    }

    size_t dedicated;
    if (cmq_align16_ok(size, &dedicated) != 0) return NULL;

    if (size > (size_t)((double)b->size * 0.8)) {
        size_t need_block = dedicated;
        if (need_block < size + 16) {
            if (size > SIZE_MAX - 16) return NULL;
            need_block = size + 16;
        }
        cmq_mpool_block_t *nb = cmq_mpool_block_create(need_block);
        if (!nb) return NULL;
        uintptr_t addr = (uintptr_t)nb->mem;
        size_t aoff = (16 - (addr % 16)) % 16;
        if (aoff > SIZE_MAX - size || aoff + size > nb->size) {
            free(nb->mem);
            free(nb);
            return NULL;
        }
        b->next = nb;
        nb->next = NULL;
        pool->tail = nb;
        nb->offset = aoff;
        void *ptr = nb->mem + nb->offset;
        nb->offset += size;
        return ptr;
    }

    if (b->size > SIZE_MAX / 2) return NULL;
    size_t new_block_size = b->size * 2;
    if (new_block_size < size) {
        size_t aligned;
        if (cmq_align16_ok(size, &aligned) != 0) return NULL;
        if (aligned < size + 16) {
            if (size > SIZE_MAX - 16) return NULL;
            aligned = size + 16;
        }
        new_block_size = aligned;
    }
    cmq_mpool_block_t *nb = cmq_mpool_block_create(new_block_size);
    if (!nb) return NULL;
    uintptr_t addr = (uintptr_t)nb->mem;
    size_t aoff = (16 - (addr % 16)) % 16;
    if (aoff > SIZE_MAX - size || aoff + size > nb->size) {
        free(nb->mem);
        free(nb);
        return NULL;
    }
    b->next = nb;
    nb->next = NULL;
    pool->tail = nb;
    nb->offset = aoff;
    void *ptr = nb->mem + nb->offset;
    nb->offset += size;
    return ptr;
}

void cmq_mpool_reset(cmq_mpool_t *pool) {
    if (!pool || !pool->head) return;
    /* Free overflow/growth blocks — keep only the first block. */
    cmq_mpool_block_t *extra = pool->head->next;
    pool->head->next = NULL;
    pool->head->offset = 0;
    pool->tail = pool->head;
    while (extra) {
        cmq_mpool_block_t *n = extra->next;
        free(extra->mem);
        free(extra);
        extra = n;
    }
}

size_t cmq_mpool_used(cmq_mpool_t *pool) {
    if (!pool) return 0;
    size_t total = 0;
    cmq_mpool_block_t *cur = pool->head;
    while (cur) {
        if (cur->size > SIZE_MAX - total)
            return SIZE_MAX;
        total += cur->size;
        cur = cur->next;
    }
    return total;
}
