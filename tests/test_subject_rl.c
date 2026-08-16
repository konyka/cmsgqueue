/* N1: per-subject rate limit tests. */

#include "cmq_test.h"
#include "cmq_subject_rl.h"
#include <string.h>

TEST(subject_rl, zero_limit_admits) {
    cmq_subject_rl_t *rl = cmq_subject_rl_create(0);
    ASSERT_NOT_NULL(rl);
    ASSERT_EQ(cmq_subject_rl_check(rl, "any.subject"), 1);
    cmq_subject_rl_free(rl);
}

TEST(subject_rl, msg_limit_enforced) {
    cmq_subject_rl_t *rl = cmq_subject_rl_create(3);
    ASSERT_NOT_NULL(rl);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(cmq_subject_rl_check(rl, "foo"), 1);
    }
    ASSERT_EQ(cmq_subject_rl_check(rl, "foo"), 0);
    /* Different subject has its own bucket. */
    ASSERT_EQ(cmq_subject_rl_check(rl, "bar"), 1);
    cmq_subject_rl_free(rl);
}

TEST(subject_rl, subjects_isolated) {
    cmq_subject_rl_t *rl = cmq_subject_rl_create(2);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x.y"), 1);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x.y"), 1);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x.y"), 0);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x.z"), 1);
    ASSERT_EQ(cmq_subject_rl_check(rl, "y.z"), 1);
    cmq_subject_rl_free(rl);
}

TEST_MAIN()
