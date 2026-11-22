#ifndef ROBCO_BOOT_H
#define ROBCO_BOOT_H

#include <stdint.h>

/* Registers and CPU mode (CPSR bits 4:0) at image entry. */
typedef struct {
    uint32_t r0, r1, r2, r3, entry_mode;
} robco_boot_args_t;

const robco_boot_args_t *robco_boot_args(void);

/* Masks IRQ/FIQ and waits forever. */
void robco_halt(void) __attribute__((noreturn));

/* Called by start.S. Detects the board, then calls robco_main(). */
void robco_start(void);

/* Provided by the project. */
void robco_main(void);

#endif
