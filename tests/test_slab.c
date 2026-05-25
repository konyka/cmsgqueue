#include "cmq_slab.h"
#include "cmq_test.h"

TEST(slab, create_destroy) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 64);
    ASSERT_NOT_NULL(slab);
    cmq_slab_destroy(slab);
}

TEST(slab, basic_alloc) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 64);
    int *p = (int *)cmq_slab_alloc(slab);
    ASSERT_NOT_NULL(p);
    *p = 42;
    ASSERT_EQ(*p, 42);
    cmq_slab_destroy(slab);
}

TEST(slab, alloc_many) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 4);
    int *ptrs[16];
    for (int i = 0; i < 16; i++) {
        ptrs[i] = (int *)cmq_slab_alloc(slab);
        ASSERT_NOT_NULL(ptrs[i]);
        *ptrs[i] = i;
    }
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(*ptrs[i], i);
    }
    cmq_slab_destroy(slab);
}

TEST(slab, alloc_free_cycle) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 4);
    void *p1 = cmq_slab_alloc(slab);
    ASSERT_NOT_NULL(p1);
    cmq_slab_free(slab, p1);
    void *p2 = cmq_slab_alloc(slab);
    ASSERT_NOT_NULL(p2);
    /* should reuse freed slot */
    ASSERT(p2 == p1);
    cmq_slab_destroy(slab);
}

TEST(slab, free_null_safe) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 4);
    cmq_slab_free(slab, NULL);
    cmq_slab_destroy(slab);
}

TEST(slab, count) {
    cmq_slab_t *slab = cmq_slab_create(sizeof(int), 4);
    ASSERT_EQ(cmq_slab_count(slab), (size_t)0);
    cmq_slab_alloc(slab);
    ASSERT_EQ(cmq_slab_count(slab), (size_t)1);
    cmq_slab_alloc(slab);
    ASSERT_EQ(cmq_slab_count(slab), (size_t)2);
    cmq_slab_destroy(slab);
}

TEST_MAIN()
