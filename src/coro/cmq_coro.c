#define _GNU_SOURCE
#include "cmq_coro.h"
#include <stdlib.h>
#include <string.h>

static void cmq_coro_trampoline(void);

static __thread cmq_coro_t *cmq_current_coro = NULL;

cmq_coro_t *cmq_coro_current(void) {
    return cmq_current_coro;
}

static void cmq_coro_set_current(cmq_coro_t *coro) {
    cmq_current_coro = coro;
}

cmq_coro_t *cmq_coro_create(cmq_coro_func_t func, void *arg, size_t stack_size) {
    if (stack_size < 4096) stack_size = 4096;

    cmq_coro_t *coro = (cmq_coro_t *)malloc(sizeof(cmq_coro_t));
    if (!coro) return NULL;
    memset(coro, 0, sizeof(*coro));

    coro->func = func;
    coro->arg = arg;
    coro->stack_size = stack_size;
    coro->state = CMQ_CORO_READY;

    void *stack_base = NULL;
    if (posix_memalign(&stack_base, 16, stack_size) != 0 || !stack_base) {
        free(coro);
        return NULL;
    }
    coro->stack_base = stack_base;

    uintptr_t end = (uintptr_t)stack_base + stack_size;
    uintptr_t *sp = (uintptr_t *)(end - 56);
    sp = (uintptr_t *)((uintptr_t)sp & ~(uintptr_t)0xF);

    for (int i = 0; i < 6; ++i) sp[i] = 0;
    sp[6] = (uintptr_t)cmq_coro_trampoline;

    coro->ctx_sp = sp;
    coro->caller_sp = NULL;

    return coro;
}

void cmq_coro_destroy(cmq_coro_t *coro) {
    if (!coro) return;
    free(coro->stack_base);
    free(coro);
}

void cmq_coro_resume(cmq_coro_t *coro) {
    if (!coro) return;
    if (coro->state != CMQ_CORO_READY && coro->state != CMQ_CORO_SUSPENDED)
        return;

    cmq_coro_t *prev = cmq_current_coro;
    cmq_coro_set_current(coro);
    coro->state = CMQ_CORO_RUNNING;

    cmq_coro_ctx_switch(&coro->caller_sp, coro->ctx_sp);

    cmq_coro_set_current(prev);
}

void cmq_coro_yield(void) {
    cmq_coro_t *coro = cmq_current_coro;
    if (!coro) return;
    coro->state = CMQ_CORO_SUSPENDED;
    cmq_coro_ctx_switch(&coro->ctx_sp, coro->caller_sp);
}

cmq_coro_state_t cmq_coro_state(cmq_coro_t *coro) {
    if (!coro) return CMQ_CORO_DONE;
    return coro->state;
}

static void cmq_coro_trampoline(void) {
    cmq_coro_t *coro = cmq_current_coro;
    if (coro && coro->func) {
        coro->func(coro->arg);
    }
    if (coro) {
        coro->state = CMQ_CORO_DONE;
        cmq_coro_ctx_switch(&coro->ctx_sp, coro->caller_sp);
    }
    __builtin_unreachable();
}
