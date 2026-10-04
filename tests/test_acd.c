/* v0.5.125: reload applies account_max_* manager defaults. */
#include "cmq_account.h"
#include "cmq_test.h"

TEST(acd, apply) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT(mgr != NULL);
    cmq_account_manager_set_defaults(mgr, 8, 16, 1024);
    cmq_account_manager_set_default_bytes_live(mgr, 4096);
    ASSERT_EQ(cmq_account_reload_defaults(mgr, 4, 32, 2048, 8192), 0);
    ASSERT_EQ(cmq_account_manager_default_connections(mgr), 4u);
    ASSERT_EQ(cmq_account_manager_default_subscriptions(mgr), 32u);
    ASSERT_EQ(cmq_account_manager_default_payload(mgr), 2048ull);
    ASSERT_EQ(cmq_account_manager_default_bytes_live(mgr), 8192ull);
    cmq_account_manager_destroy(mgr);
}

TEST(acd, omitted) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_manager_set_defaults(mgr, 8, 16, 1024);
    cmq_account_manager_set_default_bytes_live(mgr, 4096);
    ASSERT_EQ(cmq_account_reload_defaults(mgr, 0, 0, 0, 0), 0);
    ASSERT_EQ(cmq_account_manager_default_connections(mgr), 8u);
    ASSERT_EQ(cmq_account_manager_default_subscriptions(mgr), 16u);
    ASSERT_EQ(cmq_account_manager_default_payload(mgr), 1024ull);
    ASSERT_EQ(cmq_account_manager_default_bytes_live(mgr), 4096ull);
    cmq_account_manager_destroy(mgr);
}

TEST(acd, empty) {
    ASSERT_EQ(cmq_account_reload_defaults(NULL, 0, 0, 0, 0), 0);
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_reload_defaults(mgr, 0, 0, 0, 0), 0);
    ASSERT_EQ(cmq_account_manager_default_connections(mgr), 0u);
    cmq_account_manager_destroy(mgr);
}

TEST(acd, reject) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    cmq_account_manager_set_defaults(mgr, 8, 16, 1024);
    ASSERT(cmq_account_reload_defaults(mgr, 1000001, 0, 0, 0) != 0);
    ASSERT(cmq_account_reload_defaults(NULL, 4, 0, 0, 0) != 0);
    ASSERT_EQ(cmq_account_manager_default_connections(mgr), 8u);
    cmq_account_manager_destroy(mgr);
}

TEST_MAIN()
