#ifndef ROBCO_MMIO_H
#define ROBCO_MMIO_H

#include <stdint.h>

static inline void mmio_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline uint32_t mmio_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

/*
 * Reads a 32-bit register that may not exist.
 * Returns 0 and stores the value on success, non-zero if the read
 * took a data abort. Synchronous aborts only.
 * Not reentrant: one flag records the fault. IRQs are masked during the
 * probe, so a probe in an IRQ handler cannot run inside one in the main
 * flow. Do not probe from an abort handler.
 */
int mmio_probe(uint32_t addr, uint32_t *value);

/*
 * Barriers use the ARMv6 CP15 forms. They also work on ARMv7/v8
 * AArch32 because start.S sets SCTLR.CP15BEN on those cores.
 */
static inline void mmio_dmb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
}

static inline void mmio_dsb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

static inline void mmio_isb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" : : "r"(0) : "memory");
}

/* Busy-wait for roughly `count` loop iterations. */
static inline void mmio_delay(uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
    }
}

#endif
