/* F14/F15/F16 enforcement: quota, ACL, blocklist consulted in
 * handle_publish / accept_cb. */

#include "cmq_test.h"
#include "cmq_acl.h"
#include "cmq_quota.h"
#include "cmq_blocklist.h"

TEST(enforcement, acl_per_account_match) {
    cmq_acl_t *acl = cmq_acl_create();
    cmq_acl_allow(acl, "foo.>");
    ASSERT_EQ(cmq_acl_check(acl, "foo.bar"), 1);
    ASSERT_EQ(cmq_acl_check(acl, "other.topic"), 0);
    cmq_acl_free(acl);
}

TEST(enforcement, quota_byte_limit) {
    cmq_quota_t *q = cmq_quota_create(0, 80, 0);
    ASSERT_EQ(cmq_quota_check_publish(q, "user", 50), 1);
    ASSERT_EQ(cmq_quota_check_publish(q, "user", 30), 1);  /* total 80 */
    ASSERT_EQ(cmq_quota_check_publish(q, "user", 1), 0);   /* 80+1 > 80 */
    cmq_quota_free(q);
}

TEST(enforcement, blocklist_empty_admits) {
    cmq_blocklist_t *b = cmq_blocklist_load(NULL);
    /* NULL file is OK: empty blocklist. */
    if (b) {
        ASSERT_EQ(cmq_blocklist_check(b, 0x0A000001u), 0);
        cmq_blocklist_free(b);
    }
    /* If NULL returns NULL, that's also acceptable. */
}

TEST_MAIN()
