/*
 * void cmq_coro_ctx_switch(void **old_sp, void *new_sp);
 * x0 = old_sp
 * x1 = new_sp
 */
    .text
    .globl cmq_coro_ctx_switch
    .type cmq_coro_ctx_switch, @function
    .align 16
cmq_coro_ctx_switch:
    /* Save callee-saved registers: x19-x28, x29(fp), x30(lr) */
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    /* Save current sp */
    mov x2, sp
    str x2, [x0]

    /* Load new sp */
    mov sp, x1

    /* Restore callee-saved registers */
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #96

    ret

    .size cmq_coro_ctx_switch, . - cmq_coro_ctx_switch
