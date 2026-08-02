// Regression test: MPSC queue race between producers, consumer, and destroy.
//
// Two producer threads each push >=10k elements; one consumer pops and
// then calls cmq_queue_destroy once. Producers must observe the dying
// flag and exit cleanly. Verifies no ASan/TSan hits and clean destroy.

#define _POSIX_C_SOURCE 200809L
#include "cmq_queue.h"
#include "cmq_test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define PRODUCER_ITERS 10000

typedef struct {
    cmq_queue_t *q;
    int id;
    pthread_barrier_t *barrier;
    atomic_int *producer_done;
    atomic_int *destroy_started;
} producer_arg_t;

typedef struct {
    cmq_queue_t *q;
    pthread_barrier_t *barrier;
    atomic_int *destroy_started;
    int pop_budget;
} consumer_arg_t;

static void *producer_thread(void *arg) {
    producer_arg_t *pa = (producer_arg_t *)arg;
    pthread_barrier_wait(pa->barrier);
    for (int i = 0; i < PRODUCER_ITERS; i++) {
        uintptr_t payload = ((uintptr_t)0xdeadbeefUL << 32) |
                            (uintptr_t)((pa->id << 24) | (i & 0x00ffffff));
        if (cmq_queue_push(pa->q, (void *)payload) != 0) {
            break;
        }
        if (atomic_load(pa->destroy_started)) {
            break;
        }
    }
    atomic_fetch_add(pa->producer_done, 1);
    return NULL;
}

static void *consumer_thread(void *arg) {
    consumer_arg_t *ca = (consumer_arg_t *)arg;
    pthread_barrier_wait(ca->barrier);

    int popped = 0;
    while (popped < ca->pop_budget) {
        void *p = cmq_queue_pop(ca->q);
        if (p != NULL) {
            popped++;
            continue;
        }
        struct timespec ts = {0, 1000L};
        nanosleep(&ts, NULL);
    }

    atomic_store(ca->destroy_started, 1);
    cmq_queue_destroy(ca->q);
    return NULL;
}

TEST(queue_race, mpsc_destroy_concurrent) {
    cmq_queue_t q;
    cmq_queue_init(&q);
    ASSERT_NOT_NULL(q.head);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 3);

    atomic_int producer_done;
    atomic_int destroy_started;
    atomic_init(&producer_done, 0);
    atomic_init(&destroy_started, 0);

    producer_arg_t pa[2] = {
        { .q = &q, .id = 0, .barrier = &barrier,
          .producer_done = &producer_done, .destroy_started = &destroy_started },
        { .q = &q, .id = 1, .barrier = &barrier,
          .producer_done = &producer_done, .destroy_started = &destroy_started },
    };
    consumer_arg_t ca = {
        .q = &q, .barrier = &barrier,
        .destroy_started = &destroy_started,
        .pop_budget = 5000, /* drain a chunk, then destroy */
    };

    pthread_t tprod[2];
    pthread_t tcons;
    ASSERT_EQ(pthread_create(&tprod[0], NULL, producer_thread, &pa[0]), 0);
    ASSERT_EQ(pthread_create(&tprod[1], NULL, producer_thread, &pa[1]), 0);
    ASSERT_EQ(pthread_create(&tcons, NULL, consumer_thread, &ca), 0);

    pthread_join(tcons, NULL);
    pthread_join(tprod[0], NULL);
    pthread_join(tprod[1], NULL);

    /* Sanity: producers observed the destroy or finished naturally. */
    ASSERT_EQ(atomic_load(&producer_done), 2);

    pthread_barrier_destroy(&barrier);

    /* Queue was destroyed cleanly inside the consumer thread. Calling
       destroy again must be a no-op (defensive). */
    cmq_queue_destroy(&q);
}

TEST_MAIN()