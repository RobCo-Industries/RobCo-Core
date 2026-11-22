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
 * interrupt sources masked at the controller, system timer channels 1
 * and 3 parked with their match flags cleared, the I-cache invalidated and
 * the MMU and D-cache off. Registers: r0 = 0, r1 = the machine id saved at
 * entry, r2 = fdt_or_atags, r3 = bi. Flush the UART first.
 *
 * Cache contract: the next stage starts with the MMU, D-cache and I-cache
 * off and every dirty line written to memory, whatever the caller had on.
 * If the MMU or D-cache is on, the function calls mmu_disable(), which
 * cleans and invalidates the whole D-cache on every supported CPU. An
 * image that turned the caches on without mmu.h must clean them itself
 * on ARMv7 and later: the fallback cleans only on ARM1176.
 */
void robco_handoff(uint32_t entry, const struct robco_bootinfo *bi, uint32_t fdt_or_atags)
    __attribute__((noreturn));

/* Provided by the project. */
void robco_main(void);

#endif
