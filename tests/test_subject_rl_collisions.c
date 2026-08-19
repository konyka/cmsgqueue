/* P2: subject_rl collision resistance. Construct many subjects and
 * assert admitted count does not exceed the configured limit. FNV-1a
 * hash collisions cannot bypass the limit because the comparison
 * falls back to a full strcmp on the actual subject. */

#include "cmq_test.h"
#include "cmq_subject_rl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RL_MAX 50

TEST(subject_rl, hash_collision_does_not_bypass_limit) {
    cmq_subject_rl_t *rl = cmq_subject_rl_create(RL_MAX);
    ASSERT_NOT_NULL(rl);

    /* Generate a wide range of subjects with deliberate variety:
     * the FNV-1a hash should cluster some of them into the same
     * bucket, exercising the collision path. */
    int admitted = 0;
    for (int i = 0; i < 1000; i++) {
        char subj[64];
        snprintf(subj, sizeof(subj), "perf.subject.%d.%c", i,
                  'a' + (i % 26));
        if (cmq_subject_rl_check(rl, subj) == 1) admitted++;
    }
    /* The limit applies to a single subject. With distinct subjects,
     * the limit is irrelevant — every subject is admitted. The test
     * is to verify no crash and no over-admission due to collisions. */
    ASSERT_EQ(admitted, 1000);

    /* Single subject under load: limit is enforced. */
    cmq_subject_rl_t *rl2 = cmq_subject_rl_create(RL_MAX);
    int same_admitted = 0;
    for (int i = 0; i < 1000; i++) {
        if (cmq_subject_rl_check(rl2, "single.subject") == 1) {
            same_admitted++;
        }
    }
    printf("  same_subject admitted=%d (expect %d)\n", same_admitted, RL_MAX);
    ASSERT(same_admitted <= RL_MAX);
    ASSERT(same_admitted > 0);
    cmq_subject_rl_free(rl2);
    cmq_subject_rl_free(rl);
}

TEST(subject_rl, exact_limit_boundary) {
    cmq_subject_rl_t *rl = cmq_subject_rl_create(3);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x"), 1);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x"), 1);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x"), 1);
    ASSERT_EQ(cmq_subject_rl_check(rl, "x"), 0);  /* 4th rejected */
    ASSERT_EQ(cmq_subject_rl_check(rl, "y"), 1);  /* new subject admitted */
    cmq_subject_rl_free(rl);
}

TEST_MAIN()