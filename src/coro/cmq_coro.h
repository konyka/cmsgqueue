#ifndef CMQ_CORO_H
#define CMQ_CORO_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CMQ_CORO_READY = 0,
    CMQ_CORO_RUNNING = 1,
    CMQ_CORO_SUSPENDED = 2,
    CMQ_CORO_DONE = 3
} cmq_coro_state_t;

typedef void (*cmq_coro_func_t)(void *arg);

typedef struct cmq_coro {
    void *stack_base;
    size_t stack_size;
    void *ctx_sp;
    void *caller_sp;
    cmq_coro_func_t func;
    void *arg;
    cmq_coro_state_t state;
} cmq_coro_t;

cmq_coro_t *cmq_coro_create(cmq_coro_func_t func, void *arg, size_t stack_size);
void cmq_coro_destroy(cmq_coro_t *coro);
void cmq_coro_resume(cmq_coro_t *coro);
void cmq_coro_yield(void);
cmq_coro_state_t cmq_coro_state(cmq_coro_t *coro);
cmq_coro_t *cmq_coro_current(void);

void cmq_coro_ctx_switch(void **old_sp, void *new_sp);

#endif
