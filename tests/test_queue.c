#include "cmq_queue.h"
#include "cmq_test.h"
#include <string.h>

TEST(queue, destroy_empty) {
    cmq_queue_t q;
    cmq_queue_init(&q);
    cmq_queue_destroy(&q);
}

TEST(queue, destroy_after_push_pop) {
    cmq_queue_t q;
    cmq_queue_init(&q);
    int a = 1, b = 2;
    ASSERT_EQ(cmq_queue_push(&q, &a), 0);
    ASSERT_EQ(cmq_queue_push(&q, &b), 0);
    ASSERT_EQ(cmq_queue_pop(&q), (void *)&a);
    ASSERT_EQ(cmq_queue_pop(&q), (void *)&b);
    ASSERT_EQ(cmq_queue_pop(&q), NULL);
    cmq_queue_destroy(&q); /* must not double-free sentinel */
}

TEST(queue, destroy_with_pending) {
    cmq_queue_t q;
    cmq_queue_init(&q);
    int a = 1, b = 2;
    ASSERT_EQ(cmq_queue_push(&q, &a), 0);
    ASSERT_EQ(cmq_queue_push(&q, &b), 0);
    cmq_queue_destroy(&q); /* drain + free remaining sentinel once */
}

TEST(queue, push_uninit_fails) {
    cmq_queue_t q;
    memset(&q, 0, sizeof(q));
    int x = 1;
    ASSERT_EQ(cmq_queue_push(&q, &x), -1);
}

TEST_MAIN()
