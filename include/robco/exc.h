#ifndef ROBCO_EXC_H
#define ROBCO_EXC_H

#ifndef ROBCO_STAGE_NAME
#define ROBCO_STAGE_NAME "robco"
#endif

/* Exception ids passed from the vector stubs. */
#define ROBCO_EXC_RESET     0
#define ROBCO_EXC_UNDEF     1
#define ROBCO_EXC_SVC       2
#define ROBCO_EXC_PABT      3
#define ROBCO_EXC_DABT      4
#define ROBCO_EXC_UNUSED    5
#define ROBCO_EXC_IRQ       6
#define ROBCO_EXC_FIQ       7

/* exc_frame_t layout, shared with the assembly entries. */
#define EXC_FRAME_SP        52
#define EXC_FRAME_LR        56
#define EXC_FRAME_PC        60
#define EXC_FRAME_CPSR      64
#define EXC_FRAME_FSR       68
#define EXC_FRAME_FAR       72
#define EXC_FRAME_SIZE      80

/* Hook results. */
#define EXC_NOT_HANDLED     0   /* run the fatal print-and-halt path */
#define EXC_RESUME          1   /* restore every register from the frame */

#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * Register state of the interrupted context. `sp` and `lr` are the banked
 * registers of the mode in `cpsr`. `pc` is the faulting instruction for
 * undef, prefetch and data aborts (ARM or Thumb), and the instruction
 * after the `svc` for a system call. `fsr` and `far` are DFSR and DFAR for
 * a data abort, IFSR and IFAR for a prefetch abort, 0 otherwise.
 */
typedef struct exc_frame {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t fsr;
    uint32_t far;
    uint32_t pad;
} exc_frame_t;

/*
 * Called for undef, prefetch abort and data abort with the frame, in ABT
 * or UND mode with IRQs masked. Return EXC_RESUME to restore every
 * register from the (possibly modified) frame and return to frame->pc in
 * the mode of frame->cpsr, or EXC_NOT_HANDLED for the fatal path.
 * Resuming into ABT, UND, FIQ, HYP or MON mode is refused: fatal.
 */
typedef int (*exc_hook_t)(unsigned type, exc_frame_t *frame);

/*
 * Installs the hook (0 removes it) and returns the previous one. Linking
 * this function replaces the default undef and abort stubs with the
 * frame-saving entries. mmio_probe() still takes precedence.
 */
exc_hook_t exc_set_hook(exc_hook_t hook);

/* Prints one "@RC ... ev=frame" line with every field of the frame. */
void exc_print_frame(const exc_frame_t *frame);

/*
 * Called by the frame-saving entries. Returns if the hook asked to
 * resume a valid mode, otherwise prints the fatal line and halts.
 */
void robco_exc_dispatch(unsigned type, exc_frame_t *frame);

/*
 * Prints one "@RC ... ev=exception" line and halts. Starts the UART first
 * if it is not initialised and the board is known. Used by the default
 * vector stubs. The FIQ and SVC stubs (robco_vec_fiq, robco_vec_svc) are
 * weak and can be replaced by a strong definition. The IRQ entry
 * (robco_vec_irq) calls robco_irq_dispatch(), which src/drv/irq_bcm.c
 * provides. Without that driver, a weak fallback in exceptions.c reports
 * the IRQ as an exception.
 */
void robco_exc_report(uint32_t type, uint32_t pc, uint32_t lr, uint32_t spsr)
    __attribute__((noreturn));
#endif

#endif
