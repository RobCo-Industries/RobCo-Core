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

#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * Prints one "@RC ... ev=exception" line and halts. Used by the default
 * vector stubs. The weak IRQ, FIQ and SVC stubs (robco_vec_irq,
 * robco_vec_fiq, robco_vec_svc) can be replaced by a strong definition.
 */
void robco_exc_report(uint32_t type, uint32_t pc, uint32_t lr, uint32_t spsr)
    __attribute__((noreturn));
#endif

#endif
