#ifndef ROBCO_CACHE_H
#define ROBCO_CACHE_H

#include <stdint.h>

/*
 * Data and instruction cache maintenance for ARM1176, Cortex-A7 and
 * Cortex-A53 (AArch32). The CPU is picked from MIDR on each call.
 * D-cache functions do nothing while SCTLR.C is clear. All are callable
 * with IRQs enabled. Range forms cover every line that overlaps
 * [start, start + size), to the point of coherency.
 */

/* Largest D-cache line of the supported CPUs. Align DMA buffers to it. */
#define CACHE_LINE_MAX  64

/* Writes dirty lines in the range back to memory. */
void cache_clean_range(const void *start, uint32_t size);

/*
 * Discards lines in the range. Lines only partly inside the range are
 * cleaned first, so bytes next to the range are never lost.
 */
void cache_invalidate_range(const void *start, uint32_t size);

/* Cleans, then invalidates the range. */
void cache_flush_range(const void *start, uint32_t size);

/* Cleans the whole D-cache, every level up to the point of coherency. */
void cache_clean_all(void);

/* Invalidates the whole I-cache and the branch predictor. */
void cache_invalidate_icache(void);

#endif
