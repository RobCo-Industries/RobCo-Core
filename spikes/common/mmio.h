#ifndef SPIKES_MMIO_H
#define SPIKES_MMIO_H

#include <stdint.h>

static inline void mmio_write(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* Busy-wait for roughly `count` loop iterations. */
static inline void delay_cycles(uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
    }
}

/* Data memory barrier, ARMv6 CP15 form (also valid on ARMv7/v8 AArch32). */
static inline void dmb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
}

#endif
