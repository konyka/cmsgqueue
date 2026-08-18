#include "cmq_test.h"
#include "cmq_rch.h"
#include <stdlib.h>

static int free_count = 0;
static void test_free(void *p) {
    (void)p;
    free_count++;
}

TEST(rch, acquire_release_basic) {
    free_count = 0;
    int dummy = 42;
    cmq_rch_t *h = cmq_rch_new(&dummy, test_free);
    ASSERT_NOT_NULL(h);
    int *p = (int *)cmq_rch_acquire(h);
    ASSERT_EQ(*p, 42);
    cmq_rch_release(h, p);
    cmq_rch_release(h, p);
    ASSERT_EQ(free_count, 1);
}

TEST(rch, swap_drops_old) {
    free_count = 0;
    int a = 1, b = 2;
    cmq_rch_t *ha = cmq_rch_new(&a, test_free);
    ASSERT_NOT_NULL(ha);
    cmq_rch_t *hb = cmq_rch_new(&b, test_free);
    ASSERT_NOT_NULL(hb);
    cmq_rch_t *prev = cmq_rch_swap(&ha, hb);
    int *p = (int *)cmq_rch_acquire(prev);
    ASSERT_EQ(*p, 1);
    cmq_rch_release(prev, p);
    ASSERT_EQ(free_count, 0);
    cmq_rch_release_owner(prev);
    ASSERT_EQ(free_count, 1);
}

TEST(rch, multiple_readers_no_free) {
    free_count = 0;
    int x = 7;
    cmq_rch_t *h = cmq_rch_new(&x, test_free);
    int *p1 = (int *)cmq_rch_acquire(h);
    int *p2 = (int *)cmq_rch_acquire(h);
    int *p3 = (int *)cmq_rch_acquire(h);
    ASSERT_EQ(*p1, 7);
    ASSERT_EQ(*p2, 7);
    ASSERT_EQ(*p3, 7);
    cmq_rch_release(h, p1);
    ASSERT_EQ(free_count, 0);
    cmq_rch_release(h, p2);
    ASSERT_EQ(free_count, 0);
    cmq_rch_release(h, p3);
    ASSERT_EQ(free_count, 0);
    cmq_rch_release_owner(h);
    ASSERT_EQ(free_count, 1);
}

TEST(rch, null_handle_safe) {
    ASSERT_NULL(cmq_rch_acquire(NULL));
    cmq_rch_release(NULL, NULL);
}

TEST_MAIN()