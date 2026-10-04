/* v0.5.124: reload applies F14 quota and N1 subject rate limits. */
#include "cmq_quota.h"
#include "cmq_subject_rl.h"
#include "cmq_test.h"

TEST(qrl, apply) {
    cmq_quota_t *q = cmq_quota_create(10, 100, 2);
    cmq_subject_rl_t *rl = cmq_subject_rl_create(20);
    ASSERT(q != NULL);
    ASSERT(rl != NULL);
    ASSERT_EQ(cmq_quota_reload(&q, 50, 1000, 4), 0);
    ASSERT_EQ(cmq_quota_max_msgs(q), 50u);
    ASSERT_EQ(cmq_quota_max_bytes(q), 1000u);
    ASSERT_EQ(cmq_quota_max_connects(q), 4u);
    ASSERT_EQ(cmq_subject_rl_reload(&rl, 80), 0);
    ASSERT_EQ(cmq_subject_rl_limit(rl), 80u);
    cmq_quota_free(q);
    cmq_subject_rl_free(rl);
}

TEST(qrl, omitted) {
    cmq_quota_t *q = cmq_quota_create(10, 100, 2);
    cmq_subject_rl_t *rl = cmq_subject_rl_create(20);
    ASSERT_EQ(cmq_quota_reload(&q, 0, 0, 0), 0);
    ASSERT_EQ(cmq_subject_rl_reload(&rl, 0), 0);
    ASSERT_EQ(cmq_quota_max_msgs(q), 10u);
    ASSERT_EQ(cmq_quota_max_bytes(q), 100u);
    ASSERT_EQ(cmq_quota_max_connects(q), 2u);
    ASSERT_EQ(cmq_subject_rl_limit(rl), 20u);
    cmq_quota_free(q);
    cmq_subject_rl_free(rl);
}

TEST(qrl, empty) {
    cmq_quota_t *q = NULL;
    cmq_subject_rl_t *rl = NULL;
    ASSERT_EQ(cmq_quota_reload(&q, 0, 0, 0), 0);
    ASSERT_EQ(cmq_subject_rl_reload(&rl, 0), 0);
    ASSERT(q == NULL);
    ASSERT(rl == NULL);
    ASSERT_EQ(cmq_quota_reload(&q, 25, 0, 0), 0);
    ASSERT(q != NULL);
    ASSERT_EQ(cmq_quota_max_msgs(q), 25u);
    ASSERT_EQ(cmq_subject_rl_reload(&rl, 40), 0);
    ASSERT(rl != NULL);
    ASSERT_EQ(cmq_subject_rl_limit(rl), 40u);
    cmq_quota_free(q);
    cmq_subject_rl_free(rl);
}

TEST(qrl, reject) {
    cmq_quota_t *q = cmq_quota_create(10, 100, 2);
    cmq_subject_rl_t *rl = cmq_subject_rl_create(20);
    ASSERT(cmq_quota_reload(&q, 10000001, 0, 0) != 0);
    ASSERT(cmq_quota_reload(&q, 0, 1073741825, 0) != 0);
    ASSERT(cmq_subject_rl_reload(&rl, 1000001) != 0);
    ASSERT_EQ(cmq_quota_max_msgs(q), 10u);
    ASSERT_EQ(cmq_subject_rl_limit(rl), 20u);
    ASSERT(cmq_quota_reload(NULL, 10, 0, 0) != 0);
    ASSERT(cmq_subject_rl_reload(NULL, 10) != 0);
    cmq_quota_free(q);
    cmq_subject_rl_free(rl);
}

TEST_MAIN()
