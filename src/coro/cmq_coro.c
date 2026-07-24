#define _GNU_SOURCE
#include "cmq_coro.h"
#include "cmq_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void cmq_coro_trampoline(void);

/* Must match cmq_coro_ctx_*.S restore frame (bytes / uintptr slots). */
#if CMQ_ARCH_AARCH64
enum { CMQ_CORO_INIT_FRAME = 96, CMQ_CORO_INIT_WORDS = 12, CMQ_CORO_LR_WORD = 11 };
#elif CMQ_ARCH_X86_64
enum { CMQ_CORO_INIT_FRAME = 56, CMQ_CORO_INIT_WORDS = 7, CMQ_CORO_LR_WORD = 6 };
#else
#error "cmq_coro: unsupported architecture"
#endif

static __thread cmq_coro_t *cmq_current_coro = NULL;

cmq_coro_t *cmq_coro_current(void) {
    return cmq_current_coro;
}

static void cmq_coro_set_current(cmq_coro_t *coro) {
    cmq_current_coro = coro;
}

cmq_coro_t *cmq_coro_create(cmq_coro_func_t func, void *arg, size_t stack_size) {
    /* Cap stack so base+size cannot wrap; keep room for initial frame. */
    enum { CMQ_CORO_STACK_MAX = 8 * 1024 * 1024 };
    if (stack_size < 4096) stack_size = 4096;
    if (stack_size > (size_t)CMQ_CORO_STACK_MAX) return NULL;

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

    uintptr_t base = (uintptr_t)stack_base;
    if (stack_size > UINTPTR_MAX - base ||
        stack_size < (size_t)CMQ_CORO_INIT_FRAME + 16u) {
        free(stack_base);
        free(coro);
        return NULL;
    }
    uintptr_t end = base + stack_size;
    uintptr_t *sp = (uintptr_t *)(end - (size_t)CMQ_CORO_INIT_FRAME);
    sp = (uintptr_t *)((uintptr_t)sp & ~(uintptr_t)0xF);
    if ((uintptr_t)sp < base ||
        (uintptr_t)(sp + CMQ_CORO_INIT_WORDS) > end) {
        free(stack_base);
        free(coro);
        return NULL;
    }

    for (int i = 0; i < CMQ_CORO_INIT_WORDS; ++i)
        sp[i] = 0;
    sp[CMQ_CORO_LR_WORD] = (uintptr_t)cmq_coro_trampoline;

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
