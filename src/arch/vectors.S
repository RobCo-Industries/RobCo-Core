/*
 * Exception vectors, default stubs and mmio_probe().
 * Every stub ends in robco_exc_common: r0 = exception id, r1 = faulting PC.
 */

#include <robco/exc.h>

#define MODE_MASK   0x1f
#define MODE_USR    0x10
#define MODE_SYS    0x1f
#define MODE_MON    0x16
#define MODE_HYP    0x1a

/* VBAR needs 32-byte alignment. */
    .section .text.vectors, "ax"
    .arm
    .balign 32
    .global robco_vectors
robco_vectors:
    ldr     pc, =robco_vec_reset
    ldr     pc, =robco_vec_undef
    ldr     pc, =robco_vec_svc
    ldr     pc, =robco_vec_pabt
    ldr     pc, =robco_vec_dabt
    ldr     pc, =robco_vec_unused
    ldr     pc, =robco_vec_irq
    ldr     pc, =robco_vec_fiq
    .ltorg

robco_vec_reset:
    mov     r0, #ROBCO_EXC_RESET
    mov     r1, #0
    b       robco_exc_common
robco_vec_undef:
    mov     r0, #ROBCO_EXC_UNDEF
    sub     r1, lr, #4
    b       robco_exc_common
robco_vec_pabt:
    mov     r0, #ROBCO_EXC_PABT
    sub     r1, lr, #4
    b       robco_exc_common
robco_vec_unused:
    mov     r0, #ROBCO_EXC_UNUSED
    mov     r1, lr
    b       robco_exc_common

/*
 * Data abort. A fault on the probe load is expected: flag it and resume
 * at the next instruction. Anything else is reported.
 */
robco_vec_dabt:
    sub     lr, lr, #8                      @ faulting instruction
    push    {r0, r1}
    ldr     r0, =robco_probe_insn
    cmp     lr, r0
    bne     1f
    ldr     r0, =robco_probe_faulted
    mov     r1, #1
    str     r1, [r0]
    pop     {r0, r1}
    add     lr, lr, #4
    movs    pc, lr                          @ also restores CPSR from SPSR
1:
    pop     {r0, r1}
    mov     r0, #ROBCO_EXC_DABT
    mov     r1, lr
    b       robco_exc_common

/* Weak so a later FIQ or SVC component can replace them. */
    .weak   robco_vec_svc
robco_vec_svc:
    mov     r0, #ROBCO_EXC_SVC
    sub     r1, lr, #4
    b       robco_exc_common

/*
 * IRQ entry. Saves the caller-saved registers, lr and spsr on the IRQ
 * stack, calls robco_irq_dispatch(pc, spsr) and returns to the
 * interrupted code. IRQs stay masked throughout: no nesting. Eight
 * words are pushed so sp keeps the 8-byte alignment the AAPCS needs.
 */
robco_vec_irq:
    push    {r0-r3, r12, lr}
    mrs     r0, spsr
    push    {r0, r1}                        @ r1 is padding
    sub     r0, lr, #4                      @ interrupted instruction
    mrs     r1, spsr
    bl      robco_irq_dispatch
    pop     {r0, r1}
    msr     spsr_cxsf, r0
    pop     {r0-r3, r12, lr}
    subs    pc, lr, #4                      @ also restores CPSR from SPSR
    .weak   robco_vec_fiq
robco_vec_fiq:
    mov     r0, #ROBCO_EXC_FIQ
    sub     r1, lr, #4

    .global robco_exc_common
robco_exc_common:
    cpsid   if
    mrs     r3, spsr
    /*
     * Read the interrupted mode's banked LR by switching to that mode.
     * USR shares SYS registers. HYP and MON cannot be entered from here.
     */
    and     r4, r3, #MODE_MASK
    cmp     r4, #MODE_USR
    moveq   r4, #MODE_SYS
    mov     r2, #0
    cmp     r4, #MODE_HYP
    cmpne   r4, #MODE_MON
    beq     2f
    mrs     r5, cpsr
    bic     r6, r5, #MODE_MASK
    orr     r6, r6, r4
    msr     cpsr_c, r6
    mov     r2, lr
    msr     cpsr_c, r5
2:
    bl      robco_exc_report                @ does not return
3:
    wfi
    b       3b
    .ltorg

/*
 * int mmio_probe(uint32_t addr, uint32_t *value)
 * The data abort stub recognises robco_probe_insn and skips it.
 */
    .section .text.mmio_probe, "ax"
    .global mmio_probe
mmio_probe:
    ldr     r2, =robco_probe_faulted
    mov     r3, #0
    str     r3, [r2]
    .global robco_probe_insn
robco_probe_insn:
    ldr     r12, [r0]
    ldr     r3, [r2]
    cmp     r3, #0
    movne   r0, #1
    bxne    lr
    str     r12, [r1]
    mov     r0, #0
    bx      lr
    .ltorg

    .section .bss.robco_probe_faulted, "aw", %nobits
    .balign 4
robco_probe_faulted:
    .space  4
