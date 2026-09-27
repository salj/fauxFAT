.syntax unified
.thumb
.cpu cortex-m0plus

/*
 * void xxh32_m0plus_rounds_aligned(uint32_t acc[4],
 *                                  const uint8_t *p,
 *                                  const uint8_t *end)
 *
 * p/end are 4-byte aligned and end-p is a non-zero multiple of 16.
 * Updates the four XXH32 lane accumulators in place.
 */
.section .text.xxh32_m0plus_rounds_aligned,"ax",%progbits
.global xxh32_m0plus_rounds_aligned
.type xxh32_m0plus_rounds_aligned,%function
.thumb_func
xxh32_m0plus_rounds_aligned:
    push    {r4-r7, lr}
    push    {r0}
    mov     r3, r1
    mov     lr, r2
    mov     r7, r0

    ldr     r0, [r7, #0]
    ldr     r1, [r7, #4]
    ldr     r2, [r7, #8]
    ldr     r4, [r7, #12]
    mov     r12, r4

    ldr     r5, .Lp1s
    ldr     r6, .Lp2s
    movs    r7, #19

    .p2align 2
.Lloops:
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
    bne     .Lloops

    pop     {r7}
    str     r0, [r7, #0]
    str     r1, [r7, #4]
    str     r2, [r7, #8]
    mov     r4, r12
    str     r4, [r7, #12]
    pop     {r4-r7, pc}

.p2align 2
.Lp1s: .word 0x9e3779b1
.Lp2s: .word 0x85ebca77
.size xxh32_m0plus_rounds_aligned, .-xxh32_m0plus_rounds_aligned
