#ifndef ROBCO_MMU_H
#define ROBCO_MMU_H

#include <stdint.h>

/*
 * Identity map of 1 MiB sections in one 16 KiB level-1 table. Virtual
 * address equals physical address everywhere. Core 0 only.
 */

/* Memory types for mmu_map(). Every mapped section is privileged-only by default. */
#define MMU_ATTR_NONE       0   /* unmapped: any access faults */
#define MMU_ATTR_RAM        1   /* normal, write-back write-allocate, executable */
#define MMU_ATTR_DEVICE     2   /* shareable device, never cached, execute-never */
#define MMU_ATTR_FB         3   /* normal non-cacheable (bufferable), execute-never */
#define MMU_ATTR_RO_CODE    4   /* normal write-back, read-only, executable */

#define MMU_ATTR_TYPE_MASK  0xffu

/*
 * Permission flags, ORed into the type. Without one, a section is for the
 * privileged modes only: any USR-mode access faults. MMU_ATTR_RO_CODE
 * stays privileged read-only. Valid combinations:
 *   MMU_ATTR_RAM | MMU_USER_RW       user read-write, executable
 *   MMU_ATTR_RAM | MMU_USER_RO       user read-only, executable, privileged read-write
 *   MMU_ATTR_RO_CODE | MMU_USER_RO   read-only for both, executable
 * Any other combination returns MMU_ERR_ARG (device and framebuffer
 * memory never take a user flag).
 */
#define MMU_USER_RW         0x100u
#define MMU_USER_RO         0x200u

#define MMU_SECTION_SIZE    0x00100000u

#define MMU_ERR_BOARD   (-1)    /* unknown board or no RAM size from the mailbox */
#define MMU_ERR_ARG     (-2)    /* bad range or attribute */
#define MMU_ERR_ON      (-3)    /* mmu_init() while the MMU is on */

/*
 * Builds the default map: board RAM as MMU_ATTR_RAM, the peripheral window
 * (and the 0x40000000 local peripherals on BCM2836/7) as MMU_ATTR_DEVICE,
 * the framebuffer as MMU_ATTR_FB if fb_init() already ran, the rest
 * unmapped. Does not enable the MMU. Fails while the MMU is on.
 */
int mmu_init(void);

/*
 * Sets every 1 MiB section that overlaps [start, start + size) to `attr`,
 * a memory type optionally ORed with one MMU_USER_* flag.
 * Works before and after mmu_enable(). With the MMU on it cleans cached
 * data out of sections that stop being cacheable, then invalidates the
 * TLB and branch predictor. Main context; masks IRQs while it runs.
 */
int mmu_map(uint32_t start, uint32_t size, unsigned attr);

/*
 * Sets the SMP bit on Cortex-A7/A53 if clear, invalidates caches and TLB,
 * then turns on the MMU, D-cache, I-cache and branch prediction. Does
 * nothing if mmu_init() has not succeeded or the MMU is already on.
 */
void mmu_enable(void);

/*
 * Cleans and invalidates the D-cache, then turns off the D-cache, I-cache
 * and MMU. Safe from cached code. Leaves IRQs masked as they were.
 */
void mmu_disable(void);

/* Non-zero when SCTLR.M is set. */
int mmu_enabled(void);

/*
 * SMP bit after mmu_enable(): 1 set, 0 clear (the write trapped or was
 * ignored), -1 on ARM1176, which has none.
 */
int mmu_smp(void);

#endif
