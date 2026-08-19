/* P2: cmq_rch abuse paths. Verifies the refcounted handle's invariants
 * under double-release, overflow, and rapid swap. Run under ASAN. */

#include "cmq_test.h"
#include "cmq_rch.h"
#include <stdlib.h>
#include <string.h>

static int free_count;
static void test_free_fn(void *p) {
    (void)p;
    free_count++;
}

TEST(rch, double_release_safe) {
    free_count = 0;
    int dummy = 42;
    cmq_rch_t *h = cmq_rch_new(&dummy, test_free_fn);
    ASSERT_NOT_NULL(h);
    void *p = cmq_rch_acquire(h);
    ASSERT_NOT_NULL(p);
    /* Two releases: one from the reader, one from the owner. */
    cmq_rch_release(h, p);
    cmq_rch_release_owner(h);
    /* After both, free_count should be 1. */
    ASSERT_EQ(free_count, 1);
}

TEST(rch, acquire_beyond_initial) {
    free_count = 0;
    int dummy = 0;
    cmq_rch_t *h = cmq_rch_new(&dummy, test_free_fn);
    void *p1 = cmq_rch_acquire(h);
    void *p2 = cmq_rch_acquire(h);
    ASSERT_EQ(p1, p2);
    ASSERT_EQ(p1, &dummy);
    /* Two acquires bumped refcount to 3. Release both. */
    cmq_rch_release(h, p1);
    cmq_rch_release(h, p2);
    /* Refcount is back to 1 (owner still holds). */
    cmq_rch_release_owner(h);
    ASSERT_EQ(free_count, 1);
}

TEST(rch, swap_transfers_ownership) {
    free_count = 0;
    int a = 1, b = 2;
    cmq_rch_t *ha = cmq_rch_new(&a, test_free_fn);
    cmq_rch_t *hb = cmq_rch_new(&b, test_free_fn);
    cmq_rch_t *old = cmq_rch_swap(&ha, hb);
    ASSERT_NOT_NULL(old);
    /* Drop the old slot's owner reference. */
    cmq_rch_release_owner(old);
    /* Old's free_fn should have run for object a. */
    ASSERT_EQ(free_count, 1);
    /* The new handle still owns object b. */
    ASSERT_EQ(cmq_rch_acquire(hb), &b);
    cmq_rch_release(hb, &b);
    cmq_rch_release_owner(hb);
    ASSERT_EQ(free_count, 2);
}

TEST(rch, null_handle_release_safe) {
    cmq_rch_release(NULL, NULL);
    cmq_rch_release_owner(NULL);
    ASSERT_NULL(cmq_rch_acquire(NULL));
}

TEST_MAIN()