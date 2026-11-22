/*
 * Frame-saving entries for undef, prefetch abort and data abort, and
 * exc_set_hook(). They override the weak stubs in vectors.S, and only
 * when the image links exc_set_hook(): this object is otherwise left in
 * the archive.
 *
 * Each entry builds an exc_frame_t (include/robco/exc.h) on its own mode
 * stack (ABT or UND), calls robco_exc_dispatch(type, frame) with IRQs
 * masked, and, if that returns, restores every register from the frame.
 * Only ARM and Thumb return offsets are handled; the frame pc is the
 * faulting instruction in both states.
 */

#include <robco/exc.h>

#define MODE_MASK   0x1f
#define MODE_USR    0x10
#define MODE_SYS    0x1f
#define MODE_MON    0x16
#define MODE_HYP    0x1a
#define PSR_T       0x20

    .section .text.robco_exc_frame, "ax"
    .arm

/*
 * Undefined instruction. lr_und = faulting instruction + 4 (ARM) or + 2
 * (Thumb). The Thumb offset is 2 for a 32-bit Thumb-2 instruction too:
 * the CPU sets lr_und from the instruction address, not from its length
 * (TakeUndefInstrException in the ARMv7 ARM). IRQs are masked by the
 * exception entry.
 */
    .global robco_vec_undef
robco_vec_undef:
    sub     sp, sp, #EXC_FRAME_SIZE         @ frame on the UND stack
    stmia   sp, {r0-r12}                    @ r0-r12 of the interrupted context
    mrs     r1, spsr                        @ r1 = interrupted cpsr
    tst     r1, #PSR_T
    subne   r2, lr, #2                      @ Thumb: 2-byte offset
    subeq   r2, lr, #4                      @ ARM: 4-byte offset
    mov     r0, #ROBCO_EXC_UNDEF
    mov     r3, #0
    str     r3, [sp, #EXC_FRAME_FSR]
    str     r3, [sp, #EXC_FRAME_FAR]
    b       exc_frame_common

/* Prefetch abort. lr_abt = faulting instruction + 4 in both states. */
    .global robco_vec_pabt
robco_vec_pabt:
    sub     sp, sp, #EXC_FRAME_SIZE
    stmia   sp, {r0-r12}
    mrs     r1, spsr
    sub     r2, lr, #4
    mrc     p15, 0, r3, c5, c0, 1           @ IFSR
    str     r3, [sp, #EXC_FRAME_FSR]
    mrc     p15, 0, r3, c6, c0, 2           @ IFAR (ARM1176 and ARMv7)
    str     r3, [sp, #EXC_FRAME_FAR]
    mov     r0, #ROBCO_EXC_PABT
    b       exc_frame_common

/*
 * Data abort. lr_abt = faulting instruction + 8 in both states.
 * mmio_probe() first, exactly as the default stub in vectors.S: a fault
 * on robco_probe_insn from a privileged mode sets the flag and resumes
 * at the next instruction. The hook never sees it.
 */
    .global robco_vec_dabt
robco_vec_dabt:
    sub     lr, lr, #8                      @ lr = faulting instruction
    push    {r0, r1}
    ldr     r0, =robco_probe_insn
    cmp     lr, r0
    mrseq   r1, spsr
    andeq   r1, r1, #MODE_MASK
    cmpeq   r1, #MODE_USR                   @ the probe is never a USR access
    beq     1f                              @ eq: USR at the probe address, not a probe
    cmp     lr, r0
    bne     1f
    ldr     r0, =robco_probe_faulted
    mov     r1, #1
    str     r1, [r0]
    pop     {r0, r1}
    add     lr, lr, #4
    movs    pc, lr                          @ also restores CPSR from SPSR
1:
    pop     {r0, r1}                        @ all registers as at the fault again
    sub     sp, sp, #EXC_FRAME_SIZE
    stmia   sp, {r0-r12}
    mrs     r1, spsr
    mov     r2, lr
    mrc     p15, 0, r3, c5, c0, 0           @ DFSR
    str     r3, [sp, #EXC_FRAME_FSR]
    mrc     p15, 0, r3, c6, c0, 0           @ DFAR
    str     r3, [sp, #EXC_FRAME_FAR]
    mov     r0, #ROBCO_EXC_DABT
    @ fall through

/*
 * Live: r0 = type, r1 = interrupted cpsr, r2 = faulting pc, sp = frame
 * with r0-r12, fsr and far filled. r3-r12 are free (saved in the frame).
 */
exc_frame_common:
    str     r2, [sp, #EXC_FRAME_PC]
    str     r1, [sp, #EXC_FRAME_CPSR]
    /*
     * Banked sp and lr of the interrupted mode: switch to it with IRQs
     * still masked and read them. USR shares the SYS bank. HYP and MON
     * cannot be entered from here: store 0. If the interrupted mode is
     * this mode, the values read are this entry's own.
     */
    and     r3, r1, #MODE_MASK
    cmp     r3, #MODE_USR
    moveq   r3, #MODE_SYS
    mov     r6, #0
    mov     r7, #0
    cmp     r3, #MODE_HYP
    cmpne   r3, #MODE_MON
    beq     2f
    mrs     r4, cpsr                        @ r4 = this mode, I set
    bic     r5, r4, #MODE_MASK
    orr     r5, r5, r3
    msr     cpsr_c, r5                      @ now in the interrupted mode's bank
    mov     r6, sp
    mov     r7, lr
    msr     cpsr_c, r4                      @ back, our sp is the frame again
2:
    str     r6, [sp, #EXC_FRAME_SP]
    str     r7, [sp, #EXC_FRAME_LR]
    mov     r1, sp                          @ r1 = frame, r0 = type
    bl      robco_exc_dispatch              @ returns only to resume

/*
 * Resume. The frame may have been changed. robco_exc_dispatch() checked
 * that frame->cpsr names USR, SYS, SVC or IRQ, so its bank is not ours.
 */
    ldr     r1, [sp, #EXC_FRAME_CPSR]
    msr     spsr_cxsf, r1                   @ cpsr to return to
    and     r3, r1, #MODE_MASK
    cmp     r3, #MODE_USR
    moveq   r3, #MODE_SYS
    ldr     r6, [sp, #EXC_FRAME_SP]
    ldr     r7, [sp, #EXC_FRAME_LR]
    mrs     r4, cpsr
    bic     r5, r4, #MODE_MASK
    orr     r5, r5, r3
    msr     cpsr_c, r5                      @ target bank, IRQs still masked
    mov     sp, r6
    mov     lr, r7
    msr     cpsr_c, r4                      @ back to ABT or UND
    ldr     lr, [sp, #EXC_FRAME_PC]         @ lr = return address
    ldmia   sp, {r0-r12}                    @ every general register from the frame
    add     sp, sp, #EXC_FRAME_SIZE         @ drop the frame
    movs    pc, lr                          @ pc = lr, cpsr = spsr
    .ltorg

/* exc_hook_t exc_set_hook(exc_hook_t hook): returns the previous hook. */
    .global exc_set_hook
exc_set_hook:
    ldr     r2, =robco_exc_hook
    ldr     r1, [r2]
    str     r0, [r2]
    mov     r0, r1
    bx      lr
    .ltorg
