// Regression test: slab allocator churn.
//
// Allocates and frees an int-sized object 100k times from a small slab,
// verifies that cmq_slab_count returns to zero and that destroy is clean.
// Catches freelist corruption under heavy alloc/free pressure.

#define _POSIX_C_SOURCE 200809L
#include "cmq_slab.h"
#include "cmq_test.h"

#define SLAB_CHURN_ITERS 100000

TEST(slab_churn, alloc_free_loop) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 32);
    ASSERT_NOT_NULL(slab);

    for (int i = 0; i < SLAB_CHURN_ITERS; i++) {
        int *p = (int *)cmq_slab_alloc(slab);
        ASSERT_NOT_NULL(p);
        *p = i;
        cmq_slab_free(slab, p);
    }

    ASSERT_EQ(cmq_slab_count(slab), (size_t)0);
    cmq_slab_destroy(slab);
}

TEST_MAIN()