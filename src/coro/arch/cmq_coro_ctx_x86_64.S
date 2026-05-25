/*
 * void cmq_coro_ctx_switch(void **old_sp, void *new_sp);
 * rdi = old_sp (pointer to save current rsp)
 * rsi = new_sp (new rsp to switch to)
 */
    .text
    .globl cmq_coro_ctx_switch
    .type cmq_coro_ctx_switch, @function
    .align 16
cmq_coro_ctx_switch:
    /* Save callee-saved registers */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Save current stack pointer */
    movq %rsp, (%rdi)

    /* Switch to new stack */
    movq %rsi, %rsp

    /* Restore callee-saved registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret

    .size cmq_coro_ctx_switch, . - cmq_coro_ctx_switch

/* Declare for Windows compatibility */
#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
