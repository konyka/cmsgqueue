/* v0.5.49: per-account publish-side subject rewrite. */
#include "cmq_test.h"
#include "cmq_account.h"
#include <string.h>

TEST(map, no_maps_identity) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_NOT_NULL(mgr);
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_map_total(mgr), 0u);
    char out[256];
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.bar", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "foo.bar");
    ASSERT_EQ(cmq_account_map_count(mgr, "a"), (size_t)0);
    cmq_account_manager_destroy(mgr);
}

TEST(map, star_positional) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*", "bar.*"), 0);
    ASSERT_EQ(cmq_account_map_total(mgr), 1u);
    char out[256];
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.x", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "bar.x");
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.x.y", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "foo.x.y"); /* no match */
    cmq_account_manager_destroy(mgr);
}

TEST(map, gt_rest) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.>", "bar.>"), 0);
    char out[256];
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.a.b", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "bar.a.b");
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "bar");
    cmq_account_manager_destroy(mgr);
}

TEST(map, dollar) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*.bar", "acme.$1.out"), 0);
    char out[256];
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.prod.bar", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "acme.prod.out");
    cmq_account_manager_destroy(mgr);
}

TEST(map, first_match_and_upsert) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*", "one.*"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*", "two.*"), 0);
    ASSERT_EQ(cmq_account_map_count(mgr, "a"), (size_t)1);
    char out[256];
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.z", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "two.z");
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.>", "wide.>"), 0);
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "foo.z", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "two.z"); /* first remaining src still foo.* */
    cmq_account_manager_destroy(mgr);
}

TEST(map, isolated) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_create(mgr, "b"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*", "bar.*"), 0);
    char out[256];
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "b", "foo.x", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "foo.x");
    cmq_account_manager_destroy(mgr);
}

TEST(map, reject_and_remove) {
    cmq_account_manager_t *mgr = cmq_account_manager_create();
    ASSERT_EQ(cmq_account_create(mgr, "a"), 0);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.>.x", "bar.>"), -1);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo", "bar.$1"), -1);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*", "bar.>"), -1);
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "foo.*", "bar.*"), 0);
    ASSERT_EQ(cmq_account_map_total(mgr), 1u);
    ASSERT_EQ(cmq_account_remove_map(mgr, "a", "foo.*"), 0);
    ASSERT_EQ(cmq_account_map_total(mgr), 0u);
    ASSERT_EQ(cmq_account_remove_map(mgr, "a", "foo.*"), -1);
    char tiny[4];
    ASSERT_EQ(cmq_account_add_map(mgr, "a", "x", "abcd"), 0);
    ASSERT_EQ(cmq_account_rewrite_subject(mgr, "a", "x", tiny, sizeof(tiny)), -1);
    cmq_account_manager_destroy(mgr);
}

TEST_MAIN()
