// Regression test: sublist concurrent inserts and matches.
//
// Two inserter threads insert subjects "a.b.0".."a.b.49" each with a
// distinct payload per inserter. One matcher thread repeatedly calls
// cmq_sublist_match on "a.b.25" while inserters run. After joining, both
// inserter payloads must be visible at "a.b.25" — verifies rwlock path
// and absence of corruption under concurrent insert/match.

#define _POSIX_C_SOURCE 200809L
#include "cmq_sublist.h"
#include "cmq_test.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INSERTER_KEYS 50
#define RUN_SECONDS   2

typedef struct {
    cmq_sublist_t *sl;
    int id;
    atomic_int *stop;
    atomic_int *inserter_done;
} inserter_arg_t;

typedef struct {
    cmq_sublist_t *sl;
    atomic_int *stop;
} matcher_arg_t;

static void *inserter_thread(void *arg) {
    inserter_arg_t *ia = (inserter_arg_t *)arg;
    char subject[32];
    for (int i = 0; i < INSERTER_KEYS; i++) {
        if (atomic_load(ia->stop)) break;
        snprintf(subject, sizeof(subject), "a.b.%d", i);
        uintptr_t payload = ((uintptr_t)ia->id << 32) | (uintptr_t)i;
        if (cmq_sublist_insert(ia->sl, subject, (void *)payload) != 0) {
            break;
        }
    }
    atomic_fetch_add(ia->inserter_done, 1);
    return NULL;
}

static void *matcher_thread(void *arg) {
    matcher_arg_t *ma = (matcher_arg_t *)arg;
    while (!atomic_load(ma->stop)) {
        cmq_sublist_result_t result;
        if (cmq_sublist_match(ma->sl, "a.b.25", &result) != 0) {
            continue;
        }
        cmq_sublist_result_free(&result);
    }
    return NULL;
}

TEST(sublist_contention, insert_match_concurrent) {
    cmq_sublist_t *sl = cmq_sublist_create();
    ASSERT_NOT_NULL(sl);

    atomic_int stop;
    atomic_int inserter_done;
    atomic_init(&stop, 0);
    atomic_init(&inserter_done, 0);

    inserter_arg_t ia[2] = {
        { .sl = sl, .id = 0, .stop = &stop, .inserter_done = &inserter_done },
        { .sl = sl, .id = 1, .stop = &stop, .inserter_done = &inserter_done },
    };
    matcher_arg_t ma = { .sl = sl, .stop = &stop };

    pthread_t t_ins[2];
    pthread_t t_mat;
    ASSERT_EQ(pthread_create(&t_ins[0], NULL, inserter_thread, &ia[0]), 0);
    ASSERT_EQ(pthread_create(&t_ins[1], NULL, inserter_thread, &ia[1]), 0);
    ASSERT_EQ(pthread_create(&t_mat, NULL, matcher_thread, &ma), 0);

    struct timespec ts = { RUN_SECONDS, 0 };
    nanosleep(&ts, NULL);
    atomic_store(&stop, 1);

    pthread_join(t_ins[0], NULL);
    pthread_join(t_ins[1], NULL);
    pthread_join(t_mat, NULL);

    ASSERT_EQ(atomic_load(&inserter_done), 2);

    ASSERT_EQ(cmq_sublist_count(sl), (size_t)(2 * INSERTER_KEYS));

    cmq_sublist_result_t result;
    ASSERT_EQ(cmq_sublist_match(sl, "a.b.25", &result), 0);
    ASSERT_EQ(result.count, (size_t)2);
    int saw_id0 = 0, saw_id1 = 0;
    for (size_t i = 0; i < result.count; i++) {
        uintptr_t p = (uintptr_t)result.entries[i];
        int id = (int)(p >> 32);
        if (id == 0) saw_id0 = 1;
        if (id == 1) saw_id1 = 1;
    }
    ASSERT(saw_id0 && saw_id1);
    cmq_sublist_result_free(&result);

    cmq_sublist_destroy(sl);
}

TEST_MAIN()