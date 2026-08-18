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

static cmq_quota_t *g_q;
static atomic_int g_quota_admitted;

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