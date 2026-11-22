/*
 * Cache, TLB and MMU-off maintenance for ARM1176 (ARMv6), Cortex-A7
 * (ARMv7) and Cortex-A53 (ARMv8, AArch32).
 *
 * Every routine reads MIDR and picks the ARM1176 or the ARMv7 form.
 * Everything here assembles for ARMv6:
 * - The line operations by address (c7, c6/c10/c14, 1) have the same
 *   encoding on all three CPUs. Only the line size differs: 32 bytes on
 *   ARM1176, DminLine from CTR on ARMv7.
 * - The CP15 barrier forms (c7, c10, 4 DSB; c7, c5, 4 ISB) work on
 *   ARMv7/v8 because start.S sets SCTLR.CP15BEN.
 * - The ARMv7 set/way walk only uses MRC/MCR and CLZ, which ARMv6 has.
 *   It runs only after the MIDR check, so ARM1176 never reaches CLIDR.
 */

#define SCTLR_M     (1 << 0)
#define SCTLR_C     (1 << 2)
#define SCTLR_I     (1 << 12)

    .arm
    .syntax unified
    .cpu    arm1176jzf-s

/* Sets Z when the CPU is an ARM1176 (MIDR part 0xB76). */
.macro IS_ARM1176 t1, t2
    mrc     p15, 0, \t1, c0, c0, 0          @ MIDR
    mov     \t1, \t1, lsl #16
    mov     \t1, \t1, lsr #20               @ part number, bits [15:4]
    mov     \t2, #0xb00
    orr     \t2, \t2, #0x76
    cmp     \t1, \t2
.endm

/*
 * ARMv7 whole D-cache operation by set and way, every level up to the
 * level of coherency. \crm, \op2 select the operation: c6, 2 DCISW;
 * c10, 2 DCCSW; c14, 2 DCCISW. Uses r0-r5, r7, r9-r12 and no memory, so
 * it can run between a clean and the cache disable.
 */
.macro SETWAY crm, op2
    mov     r12, #0
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
    blt     4f                              @ no data cache here
    mcr     p15, 2, r10, c0, c0, 0          @ CSSELR
    mcr     p15, 0, r12, c7, c5, 4          @ ISB: CCSIDR follows CSSELR
    mrc     p15, 1, r1, c0, c0, 0           @ CCSIDR
    and     r2, r1, #7
    add     r2, r2, #4                      @ log2(line bytes)
    mov     r4, #0x400
    sub     r4, r4, #1
    ands    r4, r4, r1, lsr #3              @ max way index
    clz     r5, r4                          @ way shift (32 for one way: lsl gives 0)
    mov     r7, #0x8000
    sub     r7, r7, #1
    ands    r7, r7, r1, lsr #13             @ max set index
2:
    mov     r9, r4
3:
    orr     r11, r10, r9, lsl r5
    orr     r11, r11, r7, lsl r2
    mcr     p15, 0, r11, c7, \crm, \op2
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
    mcr     p15, 0, r12, c7, c10, 4         @ DSB
    mcr     p15, 0, r12, c7, c5, 4          @ ISB
.endm

/*
 * Range setup for void f(const void *start, uint32_t size).
 * Returns at once when SCTLR.C is clear or size is 0. Leaves r0 = start,
 * r1 = end (clamped to 0xFFFFFFFF on overflow), r2 = line size,
 * r3 = line size - 1.
 */
.macro RANGE_SETUP
    mrc     p15, 0, r2, c1, c0, 0
    tst     r2, #SCTLR_C
    bxeq    lr
    cmp     r1, #0
    bxeq    lr
    adds    r1, r0, r1
    mvncs   r1, #0
    IS_ARM1176 r2, r3
    moveq   r2, #32                         @ ARM1176: fixed 32-byte lines
    beq     6f
    mrc     p15, 0, r2, c0, c0, 1           @ CTR
    mov     r2, r2, lsr #16
    and     r2, r2, #0xf                    @ DminLine, log2(words)
    mov     r3, #4
    mov     r2, r3, lsl r2                  @ smallest D line, bytes
6:
    sub     r3, r2, #1
.endm

/* Applies one by-address line operation to every line in [r0 & ~r3, r1). */
.macro LINE_LOOP crm
    bic     r0, r0, r3
7:
    cmp     r0, r1
    bhs     8f
    mcr     p15, 0, r0, c7, \crm, 1
    adds    r0, r0, r2
    bcc     7b                              @ stop at the top of memory
8:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    bx      lr
.endm

/* void cache_clean_range(const void *start, uint32_t size) */
    .section .text.cache_clean_range, "ax"
    .global cache_clean_range
cache_clean_range:
    RANGE_SETUP
    LINE_LOOP c10                           @ DCCMVAC

/* void cache_flush_range(const void *start, uint32_t size) */
    .section .text.cache_flush_range, "ax"
    .global cache_flush_range
cache_flush_range:
    RANGE_SETUP
    LINE_LOOP c14                           @ DCCIMVAC

/*
 * void cache_invalidate_range(const void *start, uint32_t size)
 * A line only partly inside the range may hold other data that is dirty.
 * Clean and invalidate those edge lines, invalidate the rest.
 */
    .section .text.cache_invalidate_range, "ax"
    .global cache_invalidate_range
cache_invalidate_range:
    RANGE_SETUP
    tst     r0, r3
    beq     1f
    bic     r0, r0, r3
    mcr     p15, 0, r0, c7, c14, 1          @ head line: DCCIMVAC
    adds    r0, r0, r2
    bcs     2f
1:
    tst     r1, r3
    beq     9f
    bic     r1, r1, r3
    mcr     p15, 0, r1, c7, c14, 1          @ tail line: DCCIMVAC
9:
    LINE_LOOP c6                            @ DCIMVAC
2:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    bx      lr

/* void cache_clean_all(void) */
    .section .text.cache_clean_all, "ax"
    .global cache_clean_all
cache_clean_all:
    mrc     p15, 0, r0, c1, c0, 0
    tst     r0, #SCTLR_C
    bxeq    lr
    IS_ARM1176 r0, r1
    bne     1f
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 0          @ clean entire D-cache
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    bx      lr
1:
    push    {r4-r11}
    SETWAY  c10, 2                          @ DCCSW
    pop     {r4-r11}
    bx      lr

/*
 * ARM1176 erratum 411920: an I-cache invalidate can fail to invalidate
 * some lines. The workaround is four invalidates with interrupts masked,
 * then eleven NOPs. Harmless on the other CPUs, so it is used everywhere.
 * Clobbers r0, r1.
 */
.macro ICACHE_INVAL
    mov     r0, #0
    mrs     r1, cpsr
    cpsid   ifa
    mcr     p15, 0, r0, c7, c5, 0           @ ICIALLU
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 0
    msr     cpsr_c, r1
    .rept   11
    nop
    .endr
    mcr     p15, 0, r0, c7, c5, 6           @ branch predictor (BPIALL)
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
.endm

/* void cache_invalidate_icache(void) */
    .section .text.cache_invalidate_icache, "ax"
    .global cache_invalidate_icache
cache_invalidate_icache:
    ICACHE_INVAL
    bx      lr

/*
 * void robco_cache_reset_all(void)
 * For mmu_enable() only, with the D-cache off: invalidates the D-cache
 * without cleaning (ARMv7 cache contents are UNKNOWN after reset), the
 * I-cache, the branch predictor and the TLB.
 */
    .section .text.robco_cache_reset_all, "ax"
    .global robco_cache_reset_all
robco_cache_reset_all:
    push    {r4-r11}
    IS_ARM1176 r0, r1
    bne     1f
    mov     r0, #0
    mcr     p15, 0, r0, c7, c6, 0           @ invalidate entire D-cache
    b       20f
1:
    SETWAY  c6, 2                           @ DCISW
20:
    ICACHE_INVAL
    mcr     p15, 0, r0, c8, c7, 0           @ TLBIALL
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
    pop     {r4-r11}
    bx      lr

/*
 * void mmu_disable(void)
 * Runs from cached memory under the identity map. Order:
 * 1. Push the callee-saved registers. The clean in step 2 writes those
 *    stack lines back, so the final pop reads them from memory.
 * 2. Clean and invalidate the whole D-cache.
 * 3. Clear SCTLR.C, I and M. Between 2 and 3 there is no load or store,
 *    so no line can become dirty. On ARMv7 a clear C bit makes the cache
 *    invisible to data accesses, so a dirty line left there would be lost.
 * 4. Invalidate the D-cache again: speculative fills between 2 and 3 are
 *    clean, so dropping them loses nothing.
 * 5. Invalidate the I-cache, branch predictor and TLB.
 * The identity map makes the instruction after the SCTLR write the same
 * physical address with the MMU on or off.
 */
    .section .text.mmu_disable, "ax"
    .global mmu_disable
mmu_disable:
    push    {r4-r11, lr}
    mrs     r8, cpsr
    cpsid   if
    mrc     p15, 0, r0, c1, c0, 0
    tst     r0, #(SCTLR_M | SCTLR_C)
    beq     9f
    IS_ARM1176 r0, r1
    moveq   r6, #1
    movne   r6, #0
    cmp     r6, #0
    beq     11f
    mov     r0, #0
    mcr     p15, 0, r0, c7, c14, 0          @ clean and invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    b       12f
11:
    SETWAY  c14, 2                          @ DCCISW
12:
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(SCTLR_M | SCTLR_C)
    bic     r0, r0, #SCTLR_I
    mcr     p15, 0, r0, c1, c0, 0
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
    cmp     r6, #0
    beq     13f
    mcr     p15, 0, r0, c7, c6, 0           @ invalidate entire D-cache
    b       14f
13:
    SETWAY  c6, 2                           @ DCISW
14:
    ICACHE_INVAL
    mcr     p15, 0, r0, c8, c7, 0           @ TLBIALL
    mcr     p15, 0, r0, c7, c10, 4          @ DSB
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
9:
    msr     cpsr_c, r8                      @ IRQ and FIQ masks as they were
    pop     {r4-r11, pc}

/*
 * Probed access to the SMP bit registers. On Cortex-A7 and A53 these may
 * be write-protected by the secure firmware (ACTLR through NSACR, CPUECTLR
 * through ACTLR_EL3), and the access is then UNDEFINED. VBAR points at a
 * private table for the one access: its undef entry sets r0 = -1 and
 * skips the instruction. A trap to HYP or to the monitor is not caught.
 * Each returns 0 on success, -1 if the access trapped.
 */
    .section .text.robco_smp, "ax"

.macro PROBE_ENTER
    push    {r4-r6, lr}
    mov     r4, r0
    mov     r6, r1
    mrs     r12, cpsr
    cpsid   if                              @ no IRQ while VBAR is swapped
    mrc     p15, 0, r5, c12, c0, 0          @ old VBAR
    ldr     r3, =robco_smp_vectors
    mcr     p15, 0, r3, c12, c0, 0
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
.endm

.macro PROBE_EXIT
    mov     r3, #0
    mcr     p15, 0, r5, c12, c0, 0
    mcr     p15, 0, r3, c7, c5, 4           @ ISB
    msr     cpsr_c, r12
    pop     {r4-r6, pc}
.endm

/* int robco_smp_read_actlr(uint32_t *out) */
    .global robco_smp_read_actlr
robco_smp_read_actlr:
    PROBE_ENTER
    mrc     p15, 0, r1, c1, c0, 1           @ ACTLR
    cmp     r0, #0
    streq   r1, [r4]
    PROBE_EXIT

/* int robco_smp_write_actlr(uint32_t value) */
    .global robco_smp_write_actlr
robco_smp_write_actlr:
    PROBE_ENTER
    mcr     p15, 0, r4, c1, c0, 1           @ ACTLR
    PROBE_EXIT

/* int robco_smp_read_cpuectlr(uint32_t out[2]): Cortex-A53 CPUECTLR. */
    .global robco_smp_read_cpuectlr
robco_smp_read_cpuectlr:
    PROBE_ENTER
    mrrc    p15, 1, r1, r2, c15             @ CPUECTLR
    cmp     r0, #0
    streq   r1, [r4]
    streq   r2, [r4, #4]
    PROBE_EXIT

/* int robco_smp_write_cpuectlr(uint32_t lo, uint32_t hi) */
    .global robco_smp_write_cpuectlr
robco_smp_write_cpuectlr:
    PROBE_ENTER
    mcrr    p15, 1, r4, r6, c15             @ CPUECTLR
    PROBE_EXIT
    .ltorg

    .balign 32
robco_smp_vectors:
    b       robco_vectors
    b       robco_smp_undef
    b       robco_vectors + 8
    b       robco_vectors + 12
    b       robco_vectors + 16
    b       robco_vectors + 20
    b       robco_vectors + 24
    b       robco_vectors + 28
robco_smp_undef:
    mvn     r0, #0
    movs    pc, lr                          @ lr is the next instruction in ARM state
