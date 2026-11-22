/*
 * User-mode entry and exit, and the strong system-call and IRQ vectors
 * that replace the weak ones in vectors.S. This object is linked only
 * when umode.c is (umode_run() calls robco_umode_enter).
 *
 * Kernel stack while a task runs: umode_run() pushes r4-r12 and lr on
 * the SVC stack and stores sp in robco_umode_kstack. sp_svc stays at that
 * value while the task is in USR mode, so a system call builds its frame
 * just below. robco_umode_leave reloads that sp, pops and returns, from
 * any of the three ways out (system call, fault, IRQ kill).
 */

#include <robco/exc.h>

#define MODE_MASK   0x1f
#define MODE_USR    0x10
#define MODE_IRQ    0x12
#define MODE_SVC    0x13
#define MODE_SYS    0x1f
#define PSR_F       0x40

    .section .text.robco_umode, "ax"
    .arm

/*
 * int robco_umode_enter(const uint32_t ctx[7])
 * ctx: entry, sp, r0, r1, r2, r3, lr. Called in SVC mode with IRQs and
 * FIQs masked. Returns through robco_umode_leave.
 */
    .global robco_umode_enter
robco_umode_enter:
    push    {r4-r12, lr}                    @ caller's registers; 40 bytes keeps 8-byte alignment
    ldr     r1, =robco_umode_kstack
    str     sp, [r1]                        @ the leave path returns to this sp
    ldmia   r0, {r4-r10}                    @ r4 entry, r5 sp, r6-r9 r0-r3, r10 lr
    cps     #MODE_SYS                       @ SYS banks the USR sp and lr; masks unchanged
    mov     sp, r5
    mov     lr, r10                         @ return stub
    cps     #MODE_SVC
    mov     r1, #(MODE_USR | PSR_F)         @ USR, ARM state, IRQs on, FIQ masked
    msr     spsr_cxsf, r1
    mov     lr, r4                          @ entry point
    mov     r0, r6
    mov     r1, r7
    mov     r2, r8
    mov     r3, r9
    mov     r4, #0                          @ no kernel values leak to the task
    mov     r5, #0
    mov     r6, #0
    mov     r7, #0
    mov     r8, #0
    mov     r9, #0
    mov     r10, #0
    mov     r11, #0
    mov     r12, #0
    movs    pc, lr                          @ cpsr = spsr: now in USR at the entry

/*
 * Ends the task. Entered in SVC mode with IRQs masked, from the system
 * call path, from a fault (the hook makes the exception return here) or
 * from the IRQ kill path. The exit record is already filled.
 */
    .global robco_umode_leave
robco_umode_leave:
    ldr     sp, =robco_umode_kstack
    ldr     sp, [sp]                        @ SVC sp as robco_umode_enter left it
    mov     r0, #0
    pop     {r4-r12, lr}                    @ caller's registers
    bx      lr                              @ back in umode_run(), IRQs still masked

/*
 * System call. On entry: SVC mode, IRQs masked by the exception,
 * lr_svc = instruction after the svc, spsr_svc = caller's cpsr,
 * sp_svc = robco_umode_kstack if the caller is the task.
 */
    .global robco_vec_svc
robco_vec_svc:
    sub     sp, sp, #EXC_FRAME_SIZE         @ frame on the kernel stack
    stmia   sp, {r0-r12}                    @ caller's r0-r12
    mrs     r4, spsr
    str     lr, [sp, #EXC_FRAME_PC]         @ return address
    str     r4, [sp, #EXC_FRAME_CPSR]
    and     r5, r4, #MODE_MASK
    cmp     r5, #MODE_USR
    bne     svc_privileged                  @ svc from a privileged mode: fatal
    add     r0, sp, #EXC_FRAME_SP
    stmia   r0, {sp, lr}^                   @ USR sp and lr into the frame
    nop                                     @ no banked access right after stm ^
    mov     r0, #0
    str     r0, [sp, #EXC_FRAME_FSR]
    str     r0, [sp, #EXC_FRAME_FAR]
    mov     r4, sp                          @ r4 = frame, kept across the calls
    cpsie   i                               @ handlers run with IRQs on
    mov     r0, r4
    bl      robco_umode_svc_call
    cpsid   i                               @ masked from here to the return
    mov     r0, r4
    bl      robco_umode_svc_return          @ 0: resume, else end the task
    cmp     r0, #0
    bne     robco_umode_leave
    /* Resume the task from the (possibly changed) frame. */
    ldr     r0, [sp, #EXC_FRAME_CPSR]
    msr     spsr_cxsf, r0                   @ sanitised: USR, flags kept
    add     r0, sp, #EXC_FRAME_SP
    ldmia   r0, {sp, lr}^                   @ USR sp and lr
    nop
    ldr     lr, [sp, #EXC_FRAME_PC]
    ldmia   sp, {r0-r12}                    @ every other register as saved
    add     sp, sp, #EXC_FRAME_SIZE         @ kernel stack as on entry
    movs    pc, lr                          @ back to USR

svc_privileged:
    mov     r0, #ROBCO_EXC_SVC
    sub     r1, lr, #4
    b       robco_exc_common                @ prints and halts

/*
 * IRQ entry, as in vectors.S, plus the kill check: when the interrupted
 * mode is USR and a kill is pending, the task does not resume. Its
 * registers go into a frame and the IRQ returns to the kernel instead.
 * An IRQ that interrupts a system-call handler (SVC mode) returns there
 * normally; the system-call exit checks the kill request.
 */
    .global robco_vec_irq
robco_vec_irq:
    push    {r0-r3, r12, lr}                @ IRQ stack: caller-saved, lr_irq = pc + 4
    mrs     r0, spsr
    push    {r0, r1}                        @ r1 is padding
    sub     r0, lr, #4
    mrs     r1, spsr
    bl      robco_irq_dispatch
    pop     {r0, r1}
    msr     spsr_cxsf, r0
    and     r1, r0, #MODE_MASK
    cmp     r1, #MODE_USR
    bne     1f                              @ not the task: plain return
    ldr     r1, =robco_umode_kill
    ldr     r1, [r1]
    cmp     r1, #0
    bne     2f                              @ USR and kill pending: divert
1:
    pop     {r0-r3, r12, lr}
    subs    pc, lr, #4                      @ also restores CPSR from SPSR
2:
    pop     {r0-r3, r12, lr}                @ the task's registers again
    sub     lr, lr, #4                      @ lr = interrupted pc
    sub     sp, sp, #EXC_FRAME_SIZE         @ frame on the IRQ stack
    stmia   sp, {r0-r12}
    str     lr, [sp, #EXC_FRAME_PC]
    mrs     r0, spsr
    str     r0, [sp, #EXC_FRAME_CPSR]
    add     r0, sp, #EXC_FRAME_SP
    stmia   r0, {sp, lr}^                   @ USR sp and lr
    nop
    mov     r0, #0
    str     r0, [sp, #EXC_FRAME_FSR]
    str     r0, [sp, #EXC_FRAME_FAR]
    mov     r0, sp
    bl      robco_umode_irq_kill            @ records UMODE_KILLED
    add     sp, sp, #EXC_FRAME_SIZE         @ IRQ stack empty again
    cps     #MODE_SVC                       @ IRQs stay masked
    b       robco_umode_leave
    .ltorg
