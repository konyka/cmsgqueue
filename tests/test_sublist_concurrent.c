/* P2 v0.5.4: sublist_match concurrent-mutation safety. */

#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_sublist.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static cmq_sublist_t *g_sl;
static atomic_int g_stop;
static int g_match_count;

typedef struct {
    char subject[64];
    void *payload;
} test_sub_t;

static void ms_sleep(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *match_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        cmq_sublist_result_t result = {0};
        cmq_sublist_match(g_sl, "foo.bar.baz", &result);
        if (result.count > 0) g_match_count++;
        cmq_sublist_result_free(&result);
        ms_sleep(1);
    }
    return NULL;
}

static void *insert_thread(void *arg) {
    (void)arg;
    int i = 0;
    while (!atomic_load(&g_stop)) {
        test_sub_t *s = malloc(sizeof(*s));
        snprintf(s->subject, sizeof(s->subject), "foo.bar.baz");
        cmq_sublist_insert(g_sl, s->subject, s);
        ms_sleep(1);
        cmq_sublist_remove(g_sl, s->subject, s);
        free(s);
        i++;
    }
    return NULL;
}

TEST(sublist_concurrent, match_during_mutate_no_crash) {
    g_sl = cmq_sublist_create();
    ASSERT_NOT_NULL(g_sl);
    atomic_store(&g_stop, 0);
    g_match_count = 0;
    pthread_t m, i;
    pthread_create(&m, NULL, match_thread, NULL);
    pthread_create(&i, NULL, insert_thread, NULL);
    ms_sleep(200);
    atomic_store(&g_stop, 1);
    pthread_join(m, NULL);
    pthread_join(i, NULL);
    cmq_sublist_destroy(g_sl);
    printf("  successful matches during 200ms: %d\n", g_match_count);
    ASSERT(g_match_count > 0);
}

TEST_MAIN()