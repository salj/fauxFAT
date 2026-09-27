.syntax unified
.thumb
.cpu cortex-m0plus

/*
 * uint32_t xxh32_m0plus_oneshot_aligned(const uint8_t *p,
 *                                       const uint8_t *end,
 *                                       uint32_t seed)
 *
 * p and end are 4-byte aligned, end-p is a non-zero multiple of 16.
 * Returns the XXH32 four-accumulator convergence value. Length addition,
 * residual bytes, and avalanche stay in C, outside the bulk loop.
 */
.section .text.xxh32_m0plus_oneshot_aligned,"ax",%progbits
.global xxh32_m0plus_oneshot_aligned
.type xxh32_m0plus_oneshot_aligned,%function
.thumb_func
xxh32_m0plus_oneshot_aligned:
    push    {r4-r7, lr}
    mov     lr, r1
    mov     r3, r0

    ldr     r5, .Lp1
    ldr     r6, .Lp2

    mov     r0, r2
    adds    r0, r0, r5
    adds    r0, r0, r6
    mov     r1, r2
    adds    r1, r1, r6
    mov     r4, r2
    subs    r4, r4, r5
    mov     r12, r4
    movs    r7, #19

    .p2align 2
.Lloop:
    ldm     r3!, {r4}
    muls    r4, r6, r4
    adds    r0, r0, r4
    rors    r0, r7
    muls    r0, r5, r0

    ldm     r3!, {r4}
    muls    r4, r6, r4
    adds    r1, r1, r4
    rors    r1, r7
    muls    r1, r5, r1

    ldm     r3!, {r4}
    muls    r4, r6, r4
    adds    r2, r2, r4
    rors    r2, r7
    muls    r2, r5, r2

    ldm     r3!, {r4}
    muls    r4, r6, r4
    add     r4, r12
    rors    r4, r7
    muls    r4, r5, r4
    mov     r12, r4

    cmp     r3, lr
    bne     .Lloop

    movs    r7, #31
    rors    r0, r7
    movs    r7, #25
    rors    r1, r7
    adds    r0, r0, r1
    movs    r7, #20
    rors    r2, r7
    adds    r0, r0, r2
    mov     r4, r12
    movs    r7, #14
    rors    r4, r7
    adds    r0, r0, r4
    pop     {r4-r7, pc}

.p2align 2
.Lp1: .word 0x9e3779b1
.Lp2: .word 0x85ebca77
.size xxh32_m0plus_oneshot_aligned, .-xxh32_m0plus_oneshot_aligned
