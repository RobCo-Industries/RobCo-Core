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

struct robco_bootinfo;

/*
 * Enters the next stage at `entry` in SVC mode with IRQ and FIQ masked,
 * interrupt sources masked at the controller, the I-cache invalidated and
 * the MMU and D-cache off. Registers: r0 = 0, r1 = the machine id saved at
 * entry, r2 = fdt_or_atags, r3 = bi. Flush the UART first.
 */
void robco_handoff(uint32_t entry, const struct robco_bootinfo *bi, uint32_t fdt_or_atags)
    __attribute__((noreturn));

/* Provided by the project. */
void robco_main(void);

#endif
