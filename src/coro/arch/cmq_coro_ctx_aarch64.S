/*
 * void cmq_coro_ctx_switch(void **old_sp, void *new_sp);
 * x0 = old_sp
 * x1 = new_sp
 *
 * ELF (Linux) and Mach-O (macOS) differ in three ways, handled here:
 *  - Mach-O C symbols carry a leading underscore.
 *  - .type / .size are ELF-only.
 *  - .align takes a byte count on ELF but a power of two on Mach-O, so
 *    use .p2align, which means the same on both.
 */
#if defined(__APPLE__)
#define CMQ_SYM(name) _##name
#else
#define CMQ_SYM(name) name
#endif

    .text
    .globl CMQ_SYM(cmq_coro_ctx_switch)
#if defined(__ELF__)
    .type CMQ_SYM(cmq_coro_ctx_switch), @function
#endif
    .p2align 4
CMQ_SYM(cmq_coro_ctx_switch):
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

#if defined(__ELF__)
    .size CMQ_SYM(cmq_coro_ctx_switch), . - CMQ_SYM(cmq_coro_ctx_switch)
#endif

/* Declare non-executable stack for GNU ld (align x86_64). */
#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
