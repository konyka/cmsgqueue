/* F14: Quota tests. */

#include "cmq_test.h"
#include "cmq_quota.h"
#include <string.h>

TEST(quota, zero_limit_admits) {
    cmq_quota_t *q = cmq_quota_create(0, 0, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_EQ(cmq_quota_check_publish(q, "user1", 100), 1);
    ASSERT_EQ(cmq_quota_check_connect(q, "user1"), 1);
    cmq_quota_free(q);
}

TEST(quota, msg_limit_enforced) {
    cmq_quota_t *q = cmq_quota_create(3, 0, 0);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(cmq_quota_check_publish(q, "user1", 100), 1);
    }
    /* 4th message rejected. */
    ASSERT_EQ(cmq_quota_check_publish(q, "user1", 100), 0);
    cmq_quota_free(q);
}

TEST(quota, byte_limit_enforced) {
    cmq_quota_t *q = cmq_quota_create(0, 700, 0);
    /* 3 messages of 200 bytes = 600, admitted. */
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(cmq_quota_check_publish(q, "user1", 200), 1);
    }
    /* 4th message of 200 bytes: 600 + 200 = 800 > 700, rejected. */
    ASSERT_EQ(cmq_quota_check_publish(q, "user1", 200), 0);
    cmq_quota_free(q);
}

TEST(quota, accounts_isolated) {
    cmq_quota_t *q = cmq_quota_create(2, 0, 0);
    ASSERT_EQ(cmq_quota_check_publish(q, "user1", 100), 1);
    ASSERT_EQ(cmq_quota_check_publish(q, "user1", 100), 1);
    /* user1 quota exhausted; user2 not affected. */
    ASSERT_EQ(cmq_quota_check_publish(q, "user2", 100), 1);
    ASSERT_EQ(cmq_quota_check_publish(q, "user1", 100), 0);
    cmq_quota_free(q);
}

TEST(quota, connect_limit) {
    cmq_quota_t *q = cmq_quota_create(0, 0, 2);
    ASSERT_EQ(cmq_quota_check_connect(q, "user1"), 1);
    ASSERT_EQ(cmq_quota_check_connect(q, "user1"), 1);
    ASSERT_EQ(cmq_quota_check_connect(q, "user1"), 0);
    cmq_quota_free(q);
}

TEST_MAIN()
