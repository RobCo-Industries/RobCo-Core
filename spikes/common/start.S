/*
 * Universal AArch32 entry for Raspberry Pi Zero/1/2/3 (and QEMU raspi*).
 * Built for ARMv6 (ARM1176JZF-S); runs unchanged on Cortex-A7/A53.
 *
 * Firmware entry: r0 = 0, r1 = machine id, r2 = ATAGS/DTB pointer.
 * Registers kept across the boot path:
 *   r4-r6 firmware r0-r2, r7 CPSR at entry, r8 MIDR.
 */

#define MODE_MASK   0x1f
#define MODE_IRQ    0x12
#define MODE_SVC    0x13
#define MODE_ABT    0x17
#define MODE_HYP    0x1a
#define MODE_UND    0x1b
#define PSR_I       0x80
#define PSR_F       0x40
#define PART_ARM1176 0xb76

    .section .text.boot, "ax"
    .arm
    .global _start
_start:
    /* 1. Save firmware arguments and the entry CPSR. */
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    mrs     r7, cpsr

    /* 2. Park secondary cores, but only on multi-core parts. */
    mrc     p15, 0, r8, c0, c0, 0           @ MIDR
    mov     r9, r8, lsl #16
    mov     r9, r9, lsr #20                 @ part number, bits [15:4]
    ldr     r10, =PART_ARM1176
    cmp     r9, r10
    beq     1f
    mrc     p15, 0, r10, c0, c0, 5          @ MPIDR
    ands    r10, r10, #3
    bne     park
1:
    /* 3. Leave HYP mode if the firmware entered there. */
    and     r10, r7, #MODE_MASK
    cmp     r10, #MODE_HYP
    bne     2f
    bic     r10, r7, #MODE_MASK
    orr     r10, r10, #(MODE_SVC | PSR_I | PSR_F)
    /* Only reached on ARMv7+; assemble these with the A7 feature set. */
    .cpu    cortex-a7
    .arch_extension virt
    msr     SPSR_hyp, r10
    adr     r10, 2f
    msr     ELR_hyp, r10
    eret
    .cpu    arm1176jzf-s
2:
    /* Force SVC with IRQ/FIQ masked, whatever the entry mode was. */
    cpsid   if, #MODE_SVC

    /* 4. Stacks for IRQ, ABT, UND and SVC, inside the image's .stack. */
    cps     #MODE_IRQ
    ldr     sp, =__stack_irq_top
    cps     #MODE_ABT
    ldr     sp, =__stack_abt_top
    cps     #MODE_UND
    ldr     sp, =__stack_und_top
    cps     #MODE_SVC
    ldr     sp, =__stack_svc_top

    /* 5. Vector base: clear SCTLR.V (low vectors), then VBAR. */
    mrc     p15, 0, r10, c1, c0, 0
    bic     r10, r10, #(1 << 13)
    mcr     p15, 0, r10, c1, c0, 0
    ldr     r10, =vector_table
    mcr     p15, 0, r10, c12, c0, 0
    mov     r10, #0
    mcr     p15, 0, r10, c7, c5, 4          @ prefetch flush (ISB on v6)

    /* 6. Zero BSS. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
3:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     3b

    ldr     r0, =boot_entry_cpsr
    str     r7, [r0]

    mov     r0, r4
    mov     r1, r5
    mov     r2, r6
    bl      spike_main
park:
    wfi
    b       park

    .ltorg

/* Exception vectors. VBAR needs 32-byte alignment. */
    .section .text.vectors, "ax"
    .balign 32
    .global vector_table
vector_table:
    ldr     pc, =vec_reset
    ldr     pc, =vec_undef
    ldr     pc, =vec_svc
    ldr     pc, =vec_prefetch
    ldr     pc, =vec_data
    ldr     pc, =vec_unused
    ldr     pc, =vec_irq
    ldr     pc, =vec_fiq
    .ltorg

/* Each stub: r0 = exception id, r1 = faulting PC, then exc_report(). */
vec_reset:
    mov     r0, #0
    mov     r1, #0
    b       exc_common
vec_undef:
    mov     r0, #1
    sub     r1, lr, #4
    b       exc_common
vec_svc:
    mov     r0, #2
    sub     r1, lr, #4
    b       exc_common
vec_prefetch:
    mov     r0, #3
    sub     r1, lr, #4
    b       exc_common
vec_data:
    mov     r0, #4
    sub     r1, lr, #8
    b       exc_common
vec_unused:
    mov     r0, #5
    mov     r1, lr
    b       exc_common
vec_irq:
    mov     r0, #6
    sub     r1, lr, #4
    b       exc_common
vec_fiq:
    mov     r0, #7
    sub     r1, lr, #4
exc_common:
    cpsid   if
    bl      exc_report
1:
    wfi
    b       1b

    .section .data
    .balign 4
    .global boot_entry_cpsr
boot_entry_cpsr:
    .word   0
