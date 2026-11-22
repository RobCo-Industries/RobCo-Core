#ifndef SPIKES_MMU_H
#define SPIKES_MMU_H

#include <stdint.h>

struct mmu_status {
    uint32_t sctlr;       /* after enabling */
    const char *smp_reg;  /* "actlr", "cpuectlr" or "none" */
    int smp_before;       /* -1: could not read */
    int smp_after;
    const char *smp_write;/* "skipped", "ok", "trapped", "ignored" or "none" */
};

/*
 * Builds an identity map of 1 MiB sections: RAM below the peripheral base
 * and the framebuffer are normal write-back memory, the peripherals (and
 * 0x40000000 local peripherals when `local_periph`) are device memory,
 * execute-never. The rest faults.
 */
void mmu_build_identity(uintptr_t periph_base, int local_periph,
                        uint32_t fb_addr, uint32_t fb_size);

/*
 * Invalidates caches and TLBs, sets the SMP bit on Cortex-A7/A53 if needed,
 * then enables MMU, I-cache, D-cache and branch prediction.
 * `part` is the MIDR part number. Call once, with caches off.
 */
void mmu_enable(uint32_t part, struct mmu_status *st);

/* Cleans D-cache lines covering [start, end) to memory. No-op before mmu_enable. */
void mmu_clean_dcache_range(uintptr_t start, uintptr_t end);

#endif
