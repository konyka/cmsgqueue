/* F16: ACL tests. */

#include "cmq_test.h"
#include "cmq_acl.h"

TEST(acl, empty_admits_all) {
    cmq_acl_t *acl = cmq_acl_create();
    ASSERT_NOT_NULL(acl);
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "anything.else"), 1);
    cmq_acl_free(acl);
}

TEST(acl, exact_allow) {
    cmq_acl_t *acl = cmq_acl_create();
    cmq_acl_allow(acl, "foo.bar");
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "foo.baz"), 0);
    cmq_acl_free(acl);
}

TEST(acl, single_token_wildcard) {
    cmq_acl_t *acl = cmq_acl_create();
    cmq_acl_allow(acl, "foo.*");
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "foo.baz"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar.baz"), 0);  /* one token only */
    ASSERT_EQ(cmq_acl_check(acl, "other.bar"), 0);
    cmq_acl_free(acl);
}

TEST(acl, full_wildcard) {
    cmq_acl_t *acl = cmq_acl_create();
    cmq_acl_allow(acl, "foo.>");
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar.baz"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "other.bar"), 0);
    cmq_acl_free(acl);
}

TEST(acl, deny_list_wins) {
    cmq_acl_t *acl = cmq_acl_create();
    cmq_acl_allow(acl, "foo.*");
    cmq_acl_deny(acl, "foo.admin");
    /* foo.admin is denied even though foo.* allows it. */
    ASSERT_EQ(cmq_acl_check(acl, "foo.admin"), 0);
    ASSERT_EQ(cmq_acl_check(acl, "foo.user"), 1);
    cmq_acl_free(acl);
}

TEST(acl, deny_only) {
    cmq_acl_t *acl = cmq_acl_create();
    cmq_acl_deny(acl, "secret.*");
    ASSERT_EQ(cmq_acl_check(acl, "secret.password"), 0);
    ASSERT_EQ(cmq_acl_check(acl, "public.news"), 1);
    cmq_acl_free(acl);
}

TEST_MAIN()
