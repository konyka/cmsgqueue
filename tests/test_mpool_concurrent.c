#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_mpool.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define N_THREADS 4
#define N_ALLOCS  4000
#define ALLOC_SZ  96

static cmq_mpool_t *g_pool;
static void *g_slots[N_THREADS][N_ALLOCS];
static int g_failed[N_THREADS];

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < N_ALLOCS; i++) {
        uint8_t *p = cmq_mpool_alloc(g_pool, ALLOC_SZ);
        g_slots[id][i] = p;
        if (!p) { g_failed[id] = 1; continue; }
        memset(p, id + 1, ALLOC_SZ);
    }
    return NULL;
}

/* Every allocation must be a private region: after all threads finish,
 * each byte still carries the tag of the thread that wrote it. Overlap
 * between two allocations shows up as a foreign tag (and TSAN reports the
 * unsynchronised bump pointer). */
TEST(mpool_concurrent, allocations_never_overlap) {
    g_pool = cmq_mpool_create(4096);
    ASSERT_NOT_NULL(g_pool);
    memset(g_failed, 0, sizeof(g_failed));

    pthread_t t[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        ASSERT_EQ(pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i), 0);
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(t[i], NULL);

    int corrupted = 0;
    for (int id = 0; id < N_THREADS; id++) {
        ASSERT_EQ(g_failed[id], 0);
        for (int i = 0; i < N_ALLOCS; i++) {
            const uint8_t *p = g_slots[id][i];
            for (int b = 0; b < ALLOC_SZ; b++) {
                if (p[b] != (uint8_t)(id + 1)) { corrupted++; break; }
            }
        }
    }
    ASSERT_EQ(corrupted, 0);
    cmq_mpool_destroy(g_pool);
}

TEST_MAIN()
