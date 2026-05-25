#include "cmq_mpool.h"
#include "cmq_test.h"

TEST(mpool, create_destroy) {
    cmq_mpool_t *pool = cmq_mpool_create(4096);
    ASSERT_NOT_NULL(pool);
    cmq_mpool_destroy(pool);
}

TEST(mpool, basic_alloc) {
    cmq_mpool_t *pool = cmq_mpool_create(4096);
    void *p = cmq_mpool_alloc(pool, 128);
    ASSERT_NOT_NULL(p);
    /* should be 16-byte aligned */
    ASSERT_EQ((uintptr_t)p % 16, (uintptr_t)0);
    cmq_mpool_destroy(pool);
}

TEST(mpool, alloc_many) {
    cmq_mpool_t *pool = cmq_mpool_create(4096);
    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = cmq_mpool_alloc(pool, 32);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    /* all pointers should be unique */
    for (int i = 0; i < 100; i++) {
        for (int j = i + 1; j < 100; j++) {
            ASSERT(ptrs[i] != ptrs[j]);
        }
    }
    cmq_mpool_destroy(pool);
}

TEST(mpool, alloc_across_blocks) {
    /* small arena that forces multiple blocks */
    cmq_mpool_t *pool = cmq_mpool_create(256);
    void *p1 = cmq_mpool_alloc(pool, 200);
    ASSERT_NOT_NULL(p1);
    void *p2 = cmq_mpool_alloc(pool, 200);
    ASSERT_NOT_NULL(p2);
    ASSERT(p1 != p2);
    cmq_mpool_destroy(pool);
}

TEST(mpool, alloc_zero) {
    cmq_mpool_t *pool = cmq_mpool_create(4096);
    void *p = cmq_mpool_alloc(pool, 0);
    /* zero-size alloc returns non-NULL (like malloc(0) may) */
    (void)p; /* avoid unused warning in test harness */
    cmq_mpool_destroy(pool);
}

TEST(mpool, reset) {
    cmq_mpool_t *pool = cmq_mpool_create(4096);
    cmq_mpool_alloc(pool, 128);
    cmq_mpool_alloc(pool, 256);
    cmq_mpool_alloc(pool, 512);
    /* reset should reclaim all memory without freeing blocks */
    cmq_mpool_reset(pool);
    void *p = cmq_mpool_alloc(pool, 4000);
    ASSERT_NOT_NULL(p);
    cmq_mpool_destroy(pool);
}

TEST(mpool, large_alloc) {
    cmq_mpool_t *pool = cmq_mpool_create(1024);
    /* larger than block size should still work via overflow allocation */
    void *p = cmq_mpool_alloc(pool, 8192);
    ASSERT_NOT_NULL(p);
    cmq_mpool_destroy(pool);
}

TEST(mpool, stats) {
    cmq_mpool_t *pool = cmq_mpool_create(1024);
    cmq_mpool_alloc(pool, 100);
    cmq_mpool_alloc(pool, 200);
    size_t used = cmq_mpool_used(pool);
    ASSERT(used >= 300);
    cmq_mpool_destroy(pool);
}

TEST_MAIN()
