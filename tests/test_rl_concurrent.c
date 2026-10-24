/* P1: concurrent subject_rl + quota stress. The atomic fixed-slot tables
 * must admit <= max_msgs total across N worker threads contending on the
 * same key. */

#include "cmq_test.h"
#include "cmq_subject_rl.h"
#include "cmq_quota.h"

#include <pthread.h>
#include <stdatomic.h>

#define N_THREADS 4
#define N_ITER    500

static cmq_subject_rl_t *g_rl;
static atomic_int g_admitted;

static void *thread_subject_rl(void *arg) {
    (void)arg;
    for (int i = 0; i < N_ITER; i++) {
        if (cmq_subject_rl_check(g_rl, "shared.subject") == 1) {
            atomic_fetch_add(&g_admitted, 1);
        }
    }
    return NULL;
}

TEST(subject_rl, concurrent_no_bypass) {
    g_rl = cmq_subject_rl_create(50);  /* total cap = 50 msgs/sec */
    ASSERT_NOT_NULL(g_rl);
    atomic_store(&g_admitted, 0);
    pthread_t t[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        pthread_create(&t[i], NULL, thread_subject_rl, NULL);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(t[i], NULL);
    }
    int admitted = atomic_load(&g_admitted);
    printf("  admitted=%d (expect <= 50)\n", admitted);
    ASSERT(admitted <= 50);
    /* Sanity: at least one message should have been admitted. */
    ASSERT(admitted > 0);
    cmq_subject_rl_free(g_rl);
}

#define RACE_ROUNDS 3000

static atomic_int g_race_ready;
static atomic_int g_race_go;
static atomic_int g_race_admitted;

static void *thread_first_call(void *arg) {
    (void)arg;
    atomic_fetch_add(&g_race_ready, 1);
    while (!atomic_load(&g_race_go)) {
    }
    if (cmq_subject_rl_check(g_rl, "first.call.race") == 1)
        atomic_fetch_add(&g_race_admitted, 1);
    return NULL;
}

/* Every round starts with an empty table, so all threads race on the
 * very first claim of the slot. With a limit of 1, exactly one call may
 * be admitted per round no matter how the claim interleaves. */
TEST(subject_rl, first_claim_race_does_not_double_admit) {
    int violations = 0, none = 0, multi = 0;
    for (int round = 0; round < RACE_ROUNDS; round++) {
        g_rl = cmq_subject_rl_create(1);
        ASSERT_NOT_NULL(g_rl);
        atomic_store(&g_race_ready, 0);
        atomic_store(&g_race_go, 0);
        atomic_store(&g_race_admitted, 0);
        pthread_t t[N_THREADS];
        for (int i = 0; i < N_THREADS; i++)
            pthread_create(&t[i], NULL, thread_first_call, NULL);
        while (atomic_load(&g_race_ready) < N_THREADS) {
        }
        atomic_store(&g_race_go, 1);
        for (int i = 0; i < N_THREADS; i++)
            pthread_join(t[i], NULL);
        int got = atomic_load(&g_race_admitted);
        if (got != 1)
            violations++;
        if (got == 0) none++;
        if (got > 1) multi++;
        cmq_subject_rl_free(g_rl);
    }
    printf("  rounds with admitted != 1: %d / %d (none=%d multi=%d)\n",
           violations, RACE_ROUNDS, none, multi);
    ASSERT_EQ(violations, 0);
}

static cmq_quota_t *g_q;
static atomic_int g_quota_admitted;

static void *thread_quota_first_call(void *arg) {
    (void)arg;
    atomic_fetch_add(&g_race_ready, 1);
    while (!atomic_load(&g_race_go)) {
    }
    if (cmq_quota_check_publish(g_q, "first.call.acct", 10) == 1)
        atomic_fetch_add(&g_race_admitted, 1);
    return NULL;
}

TEST(quota, first_claim_race_does_not_double_admit) {
    int violations = 0, none = 0, multi = 0;
    for (int round = 0; round < RACE_ROUNDS; round++) {
        g_q = cmq_quota_create(1, 0, 0);
        ASSERT_NOT_NULL(g_q);
        atomic_store(&g_race_ready, 0);
        atomic_store(&g_race_go, 0);
        atomic_store(&g_race_admitted, 0);
        pthread_t t[N_THREADS];
        for (int i = 0; i < N_THREADS; i++)
            pthread_create(&t[i], NULL, thread_quota_first_call, NULL);
        while (atomic_load(&g_race_ready) < N_THREADS) {
        }
        atomic_store(&g_race_go, 1);
        for (int i = 0; i < N_THREADS; i++)
            pthread_join(t[i], NULL);
        int got = atomic_load(&g_race_admitted);
        if (got != 1)
            violations++;
        if (got == 0) none++;
        if (got > 1) multi++;
        cmq_quota_free(g_q);
    }
    printf("  rounds with admitted != 1: %d / %d (none=%d multi=%d)\n",
           violations, RACE_ROUNDS, none, multi);
    ASSERT_EQ(violations, 0);
}

static void *thread_quota(void *arg) {
    (void)arg;
    for (int i = 0; i < N_ITER; i++) {
        if (cmq_quota_check_publish(g_q, "shared.acct", 100) == 1) {
            atomic_fetch_add(&g_quota_admitted, 1);
        }
    }
    return NULL;
}

TEST(quota, concurrent_no_bypass) {
    g_q = cmq_quota_create(50, 0, 0);
    ASSERT_NOT_NULL(g_q);
    atomic_store(&g_quota_admitted, 0);
    pthread_t t[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        pthread_create(&t[i], NULL, thread_quota, NULL);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(t[i], NULL);
    }
    int admitted = atomic_load(&g_quota_admitted);
    printf("  admitted=%d (expect <= 50)\n", admitted);
    ASSERT(admitted <= 50);
    ASSERT(admitted > 0);
    cmq_quota_free(g_q);
}

TEST_MAIN()