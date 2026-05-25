#include "cmq_coro.h"
#include "cmq_test.h"

static void coro_basic_fn(void *arg) {
    int *counter = (int *)arg;
    (*counter)++;
    cmq_coro_yield();
    (*counter)++;
}

TEST(coro, create_destroy) {
    cmq_coro_t *coro = cmq_coro_create(coro_basic_fn, NULL, 8192);
    ASSERT_NOT_NULL(coro);
    cmq_coro_destroy(coro);
}

TEST(coro, basic_resume_yield) {
    int counter = 0;
    cmq_coro_t *coro = cmq_coro_create(coro_basic_fn, &counter, 8192);
    ASSERT_NOT_NULL(coro);

    ASSERT_EQ(cmq_coro_state(coro), CMQ_CORO_READY);

    cmq_coro_resume(coro);
    ASSERT_EQ(cmq_coro_state(coro), CMQ_CORO_SUSPENDED);
    ASSERT_EQ(counter, 1);

    cmq_coro_resume(coro);
    ASSERT_EQ(cmq_coro_state(coro), CMQ_CORO_DONE);
    ASSERT_EQ(counter, 2);

    cmq_coro_destroy(coro);
}

static void coro_nested_fn(void *arg) {
    int *val = (int *)arg;
    *val += 10;
}

static void coro_caller_fn(void *arg) {
    int *val = (int *)arg;
    *val = 1;

    cmq_coro_t *inner = cmq_coro_create(coro_nested_fn, arg, 4096);
    cmq_coro_resume(inner);
    cmq_coro_destroy(inner);

    *val += 100;
}

TEST(coro, nested_coroutines) {
    int val = 0;
    cmq_coro_t *coro = cmq_coro_create(coro_caller_fn, &val, 8192);
    cmq_coro_resume(coro);
    ASSERT_EQ(cmq_coro_state(coro), CMQ_CORO_DONE);
    ASSERT_EQ(val, 111);
    cmq_coro_destroy(coro);
}

static void coro_noop_fn(void *arg) {
    (void)arg;
}

TEST(coro, small_stack) {
    cmq_coro_t *coro = cmq_coro_create(coro_noop_fn, NULL, 4096);
    ASSERT_NOT_NULL(coro);
    cmq_coro_resume(coro);
    ASSERT_EQ(cmq_coro_state(coro), CMQ_CORO_DONE);
    cmq_coro_destroy(coro);
}

static void coro_return_fn(void *arg) {
    (void)arg;
}

TEST(coro, immediate_return) {
    cmq_coro_t *coro = cmq_coro_create(coro_return_fn, NULL, 8192);
    cmq_coro_resume(coro);
    ASSERT_EQ(cmq_coro_state(coro), CMQ_CORO_DONE);
    cmq_coro_destroy(coro);
}

TEST(coro, current_coro) {
    cmq_coro_t *c = cmq_coro_current();
    (void)c;
}

TEST_MAIN()
