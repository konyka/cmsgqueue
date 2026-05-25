#ifndef CMQ_MPOOL_H
#define CMQ_MPOOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct cmq_mpool cmq_mpool_t;

/* Create a memory pool with the given block size (0 = default 4096) */
cmq_mpool_t *cmq_mpool_create(size_t block_size);

/* Destroy the pool and free all memory */
void cmq_mpool_destroy(cmq_mpool_t *pool);

/* Allocate aligned memory from the pool (always 16-byte aligned) */
void *cmq_mpool_alloc(cmq_mpool_t *pool, size_t size);

/* Reset the pool - keeps allocated blocks but marks them as free */
void cmq_mpool_reset(cmq_mpool_t *pool);

/* Get total bytes allocated from this pool */
size_t cmq_mpool_used(cmq_mpool_t *pool);

#endif
