/*
 * Cache, TLB and MMU maintenance for spikes. The caller picks the v6
 * (ARM1176) or v7 (Cortex-A7, Cortex-A53 in AArch32) variant from MIDR.
 */

    .text
    .arm

/* ---------------------------------------------------------------- ARMv6 */
    .cpu    arm1176jzf-s

/* void mmu_v6_inval_all(void): invalidate I+D caches, BTAC and TLBs. */
    .global mmu_v6_inval_all
mmu_v6_inval_all:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c7, 0           @ invalidate I and D caches
    /* ARM1176 erratum 411920: I-cache invalidate, four times, IRQs off. */
    mrs     r1, cpsr
    cpsid   ifa
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 0
    msr     cpsr_cx, r1
    .rept   11
    nop
    .endr
    mcr     p15, 0, r0, c7, c5, 6           @ flush branch target cache
    mcr     p15, 0, r0, c8, c7, 0           @ invalidate unified TLB
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    mcr     p15, 0, r0, c7, c5, 4           @ prefetch flush
    bx      lr

/*
 * uint32_t mmu_v6_enable(uint32_t ttbr0, uint32_t set, uint32_t clear)
 * Domain 0 client, TTBCR 0, TTBR0, then SCTLR = (SCTLR & ~clear) | set.
 * Returns the new SCTLR.
 */
    .global mmu_v6_enable
mmu_v6_enable:
    mov     r3, #1
    mcr     p15, 0, r3, c3, c0, 0           @ DACR
    mov     r3, #0
    mcr     p15, 0, r3, c2, c0, 2           @ TTBCR
    mcr     p15, 0, r0, c2, c0, 0           @ TTBR0
    mcr     p15, 0, r3, c8, c7, 0           @ invalidate TLB
    mcr     p15, 0, r3, c7, c10, 4          @ DSB
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, r2
    orr     r0, r0, r1
    mcr     p15, 0, r0, c1, c0, 0
    mcr     p15, 0, r3, c7, c5, 4           @ prefetch flush
    mrc     p15, 0, r0, c1, c0, 0
    bx      lr

/* void mmu_v6_clean_range(uintptr_t start, uintptr_t end): 32-byte lines. */
    .global mmu_v6_clean_range
mmu_v6_clean_range:
    bic     r0, r0, #31
1:
    cmp     r0, r1
    bhs     2f
    mcr     p15, 0, r0, c7, c10, 1          @ clean D line by MVA
    add     r0, r0, #32
    b       1b
2:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    bx      lr

/* ---------------------------------------------------------------- ARMv7 */
    .cpu    cortex-a7

/* void mmu_v7_inval_all(void): D-cache by set/way, I-cache, BP, TLBs. */
    .global mmu_v7_inval_all
mmu_v7_inval_all:
    push    {r4-r11}
    mrc     p15, 1, r0, c0, c0, 1           @ CLIDR
    ands    r3, r0, #0x07000000
    mov     r3, r3, lsr #23                 @ LoC * 2
    beq     5f
    mov     r10, #0                         @ level * 2
1:
    add     r2, r10, r10, lsr #1            @ level * 3
    mov     r1, r0, lsr r2
    and     r1, r1, #7                      @ cache type at this level
    cmp     r1, #2
    blt     4f                              @ no data cache
    mcr     p15, 2, r10, c0, c0, 0          @ CSSELR
    isb
    mrc     p15, 1, r1, c0, c0, 0           @ CCSIDR
    and     r2, r1, #7
    add     r2, r2, #4                      @ log2(line bytes)
    ldr     r4, =0x3ff
    ands    r4, r4, r1, lsr #3              @ max way index
    clz     r5, r4                          @ way shift
    ldr     r7, =0x7fff
    ands    r7, r7, r1, lsr #13             @ max set index
2:
    mov     r9, r4
3:
    orr     r11, r10, r9, lsl r5
    orr     r11, r11, r7, lsl r2
    mcr     p15, 0, r11, c7, c6, 2          @ DCISW
    subs    r9, r9, #1
    bge     3b
    subs    r7, r7, #1
    bge     2b
4:
    add     r10, r10, #2
    cmp     r3, r10
    bgt     1b
5:
    mov     r10, #0
    mcr     p15, 2, r10, c0, c0, 0          @ CSSELR back to L1
    dsb
    isb
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0           @ ICIALLU
    mcr     p15, 0, r0, c7, c5, 6           @ BPIALL
    mcr     p15, 0, r0, c8, c7, 0           @ TLBIALL
    dsb
    isb
    pop     {r4-r11}
    bx      lr

/* uint32_t mmu_v7_enable(uint32_t ttbr0, uint32_t set, uint32_t clear) */
    .global mmu_v7_enable
mmu_v7_enable:
    mov     r3, #1
    mcr     p15, 0, r3, c3, c0, 0           @ DACR
    mov     r3, #0
    mcr     p15, 0, r3, c2, c0, 2           @ TTBCR
    mcr     p15, 0, r0, c2, c0, 0           @ TTBR0
    isb
    mcr     p15, 0, r3, c8, c7, 0           @ TLBIALL
    dsb
    isb
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, r2
    orr     r0, r0, r1
    mcr     p15, 0, r0, c1, c0, 0
    isb
    mrc     p15, 0, r0, c1, c0, 0
    bx      lr

/* void mmu_v7_clean_range(uintptr_t start, uintptr_t end): line from CTR. */
    .global mmu_v7_clean_range
mmu_v7_clean_range:
    mrc     p15, 0, r3, c0, c0, 1           @ CTR
    mov     r3, r3, lsr #16
    and     r3, r3, #0xf
    mov     r2, #4
    mov     r2, r2, lsl r3                  @ min D line, bytes
    sub     r3, r2, #1
    bic     r0, r0, r3
1:
    cmp     r0, r1
    bhs     2f
    mcr     p15, 0, r0, c7, c10, 1          @ DCCMVAC
    add     r0, r0, r2
    b       1b
2:
    dsb
    bx      lr

/*
 * Probed system register access. VBAR points at probe_vectors for one
 * instruction. If it is UNDEFINED, the handler sets r0 = -1 and skips it.
 * Each returns 0 on success, -1 if the access trapped to SVC/UND here.
 * A trap taken to HYP or the secure monitor is not caught.
 */
.macro PROBE_ENTER
    push    {r4-r6, lr}
    mov     r4, r0
    mov     r6, r1
    mrc     p15, 0, r5, c12, c0, 0          @ old VBAR
    ldr     r3, =probe_vectors
    mcr     p15, 0, r3, c12, c0, 0
    isb
    mov     r0, #0
.endm

.macro PROBE_EXIT
    mcr     p15, 0, r5, c12, c0, 0
    isb
    pop     {r4-r6, pc}
.endm

/* int mmu_v7_try_read_actlr(uint32_t *out) */
    .global mmu_v7_try_read_actlr
mmu_v7_try_read_actlr:
    PROBE_ENTER
    mrc     p15, 0, r1, c1, c0, 1
    cmp     r0, #0
    streq   r1, [r4]
    PROBE_EXIT

/* int mmu_v7_try_write_actlr(uint32_t value) */
    .global mmu_v7_try_write_actlr
mmu_v7_try_write_actlr:
    PROBE_ENTER
    mcr     p15, 0, r4, c1, c0, 1
    isb
    PROBE_EXIT

/* int mmu_v7_try_read_cpuectlr(uint32_t out[2]): Cortex-A53 CPUECTLR. */
    .global mmu_v7_try_read_cpuectlr
mmu_v7_try_read_cpuectlr:
    PROBE_ENTER
    mrrc    p15, 1, r1, r2, c15
    cmp     r0, #0
    streq   r1, [r4]
    streq   r2, [r4, #4]
    PROBE_EXIT

/* int mmu_v7_try_write_cpuectlr(uint32_t lo, uint32_t hi) */
    .global mmu_v7_try_write_cpuectlr
mmu_v7_try_write_cpuectlr:
    PROBE_ENTER
    mcrr    p15, 1, r4, r6, c15
    isb
    PROBE_EXIT

    .ltorg

    .balign 32
probe_vectors:
    b       vector_table
    b       probe_undef
    b       vector_table + 8
    b       vector_table + 12
    b       vector_table + 16
    b       vector_table + 20
    b       vector_table + 24
    b       vector_table + 28
probe_undef:
    mvn     r0, #0
    movs    pc, lr
