/* v0.5.48: per-account concurrent conn/sub/payload caps. */
#include "cmq_test.h"
#include "cmq_account.h"
#include <string.h>

TEST(limits, zero_unlimited) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_NOT_NULL(mgr);
    ASSERT_EQ(cmq_account_create(mgr, "u"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "u", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->max_connections, 0u);
    ASSERT_EQ(a->max_subscriptions, 0u);
    ASSERT_EQ(a->max_payload, (uint64_t)0);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), 0);
    ASSERT_EQ(a->connections, (uint64_t)8);
    ASSERT_EQ(cmq_account_check_payload(a, 1u << 20), 0);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(limits, conn_cap) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "c"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "c", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_limits(a, 2, 0, 0);
    ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), 0);
    ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), 0);
    ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), -2);
    ASSERT_EQ(a->connections, (uint64_t)2);
    cmq_account_dec_connections(a, a->epoch);
    ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), 0);
    ASSERT_EQ(a->connections, (uint64_t)2);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(limits, sub_cap) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "s"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "s", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_limits(a, 0, 1, 0);
    ASSERT_EQ(cmq_account_inc_subscriptions(a, a->epoch), 0);
    ASSERT_EQ(cmq_account_inc_subscriptions(a, a->epoch), -2);
    ASSERT_EQ(a->subscriptions, (uint64_t)1);
    cmq_account_dec_subscriptions(a, a->epoch);
    ASSERT_EQ(cmq_account_inc_subscriptions(a, a->epoch), 0);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(limits, payload) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "p"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "p", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(cmq_account_check_payload(a, 9999), 0);
    cmq_account_set_limits(a, 0, 0, 100);
    ASSERT_EQ(cmq_account_check_payload(a, 100), 0);
    ASSERT_EQ(cmq_account_check_payload(a, 101), -1);
    ASSERT_EQ(cmq_account_check_payload(NULL, 1), -1);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(limits, defaults_apply) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_manager_set_defaults(mgr, 3, 7, 64);
    ASSERT_EQ(cmq_account_create(mgr, "d"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "d", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->max_connections, 3u);
    ASSERT_EQ(a->max_subscriptions, 7u);
    ASSERT_EQ(a->max_payload, (uint64_t)64);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(limits, reactivate_keeps) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_manager_set_defaults(mgr, 9, 9, 9);
    ASSERT_EQ(cmq_account_create(mgr, "keep"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "keep", NULL);
    ASSERT_NOT_NULL(a);
    cmq_account_set_limits(a, 2, 4, 8);
    cmq_account_release(mgr, a);
    ASSERT_EQ(cmq_account_delete(mgr, "keep"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "keep"), 0);
    a = cmq_account_get(mgr, "keep", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->max_connections, 2u);
    ASSERT_EQ(a->max_subscriptions, 4u);
    ASSERT_EQ(a->max_payload, (uint64_t)8);
    cmq_account_release(mgr, a);
    cmq_account_manager_destroy(mgr);
}

TEST(limits, isolated) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "b"), 0);
    cmq_account_t *a = cmq_account_get(mgr, "a", NULL);
    cmq_account_t *b = cmq_account_get(mgr, "b", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    cmq_account_set_limits(a, 1, 0, 0);
    ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), 0);
    ASSERT_EQ(cmq_account_inc_connections(a, a->epoch), -2);
    ASSERT_EQ(cmq_account_inc_connections(b, b->epoch), 0);
    ASSERT_EQ(cmq_account_inc_connections(b, b->epoch), 0);
    cmq_account_release(mgr, a);
    cmq_account_release(mgr, b);
    cmq_account_manager_destroy(mgr);
}

TEST_MAIN()
