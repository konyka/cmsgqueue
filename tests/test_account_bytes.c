/* v0.5.52: per-account outstanding-byte (bytes_live) cap. */
#include "cmq_test.h"
#include "cmq_account.h"
#include <string.h>

TEST(bytes, unlimited_noop) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->max_bytes_live, (uint64_t)0);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 1000), 0);
    ASSERT_EQ(a->bytes_live, (uint64_t)0);
    cmq_account_debit_bytes_live(a, a->epoch, 1000);
    ASSERT_EQ(a->bytes_live, (uint64_t)0);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(bytes, cap_and_debit) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_max_bytes_live(a, 100);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 60), 0);
    ASSERT_EQ(a->bytes_live, (uint64_t)60);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 50), -2);
    ASSERT_EQ(a->bytes_live, (uint64_t)60);
    cmq_account_debit_bytes_live(a, a->epoch, 60);
    ASSERT_EQ(a->bytes_live, (uint64_t)0);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 50), 0);
    ASSERT_EQ(a->bytes_live, (uint64_t)50);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(bytes, exact_fill) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_max_bytes_live(a, 40);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 40), 0);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 1), -2);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(bytes, isolated) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "b"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    cmq_account_t *b = cmq_account_get(mgr, "b", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    cmq_account_set_max_bytes_live(a, 10);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 10), 0);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 1), -2);
    ASSERT_EQ(cmq_account_credit_bytes_live(b, b->epoch, 100), 0);
    ASSERT_EQ(b->bytes_live, (uint64_t)0);
    cmq_account_release(mgr, a);
    cmq_account_release(mgr, b);
    cmq_account_manager_destroy(mgr);
}

TEST(bytes, stale_epoch) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_max_bytes_live(a, 100);
    uint32_t old = a->epoch;
    ASSERT_EQ(cmq_account_delete(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, old, 10), -1);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(bytes, default_applies) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_manager_set_default_bytes_live(mgr, 77);
    ASSERT_EQ(cmq_account_create(mgr, "n"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "n", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->max_bytes_live, (uint64_t)77);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(bytes, debit_saturates) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_max_bytes_live(a, 50);
    ASSERT_EQ(cmq_account_credit_bytes_live(a, a->epoch, 20), 0);
    cmq_account_debit_bytes_live(a, a->epoch, 99);
    ASSERT_EQ(a->bytes_live, (uint64_t)0);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST_MAIN()
