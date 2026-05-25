#ifndef CMQ_SLAB_H
#define CMQ_SLAB_H

#include <stddef.h>
#include "cmq_types.h"

typedef struct cmq_slab cmq_slab_t;

/* Create a slab allocator for objects of `obj_size` with initial capacity `capacity` */
cmq_slab_t *cmq_slab_create(size_t obj_size, size_t capacity);

/* Destroy the slab and free all memory */
void cmq_slab_destroy(cmq_slab_t *slab);

/* Allocate an object from the slab */
void *cmq_slab_alloc(cmq_slab_t *slab);

/* Free an object back to the slab */
void cmq_slab_free(cmq_slab_t *slab, void *obj);

/* Get number of currently allocated objects */
size_t cmq_slab_count(cmq_slab_t *slab);

#endif
