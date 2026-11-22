#include <stdint.h>

#include <robco/board.h>
#include <robco/cache.h>
#include <robco/fb.h>
#include <robco/mmu.h>

/* Weak: mmu_init() maps the framebuffer only if the image links fb.c. */
#pragma weak fb_get

#define PART_ARM1176 0xb76
#define PART_A7      0xc07
#define PART_A53     0xd03

#define LOCAL_PERIPH 0x40000000u    /* BCM2836/7 core-local peripherals, 1 MiB */
#define PERIPH_SIZE  0x01000000u

/*
 * Short-descriptor 1 MiB section, domain 0. With TEX remap off
 * (SCTLR.TRE = 0) the memory type comes from TEX, C and B directly.
 */
#define SECT         0x2u
#define SECT_B       (1u << 2)
#define SECT_C       (1u << 3)
#define SECT_XN      (1u << 4)      /* ARM1176 needs SCTLR.XP for this bit */
#define SECT_AP0     (1u << 10)
#define SECT_AP1     (1u << 11)
#define SECT_TEX(x)  ((uint32_t)(x) << 12)
#define SECT_APX     (1u << 15)

#define AP_RW        (SECT_AP1 | SECT_AP0)          /* AP[2:0] 011: full access */
#define AP_RO        (SECT_APX | SECT_AP0)          /* AP[2:0] 101: PL1 read-only */

/* Normal, inner and outer write-back write-allocate: TEX 001, C 1, B 1. */
#define DESC_RAM     (SECT | AP_RW | SECT_TEX(1) | SECT_C | SECT_B)
/* Shareable device: TEX 000, C 0, B 1. Writes may be buffered, never merged or cached. */
#define DESC_DEVICE  (SECT | AP_RW | SECT_B | SECT_XN)
/* Normal, inner and outer non-cacheable: TEX 001, C 0, B 0. Writes merge in the write buffer. */
#define DESC_FB      (SECT | AP_RW | SECT_TEX(1) | SECT_XN)
#define DESC_RO_CODE (SECT | AP_RO | SECT_TEX(1) | SECT_C | SECT_B)

#define SCTLR_M      (1u << 0)
#define SCTLR_A      (1u << 1)
#define SCTLR_C      (1u << 2)
#define SCTLR_Z      (1u << 11)
#define SCTLR_I      (1u << 12)
#define SCTLR_XP     (1u << 23)     /* ARMv6: v6 descriptors, needed for XN and APX */
#define SCTLR_TRE    (1u << 28)
#define SCTLR_AFE    (1u << 29)

#define SMP_BIT      (1u << 6)      /* ACTLR.SMP (A7), CPUECTLR.SMPEN (A53) */

void robco_cache_reset_all(void);
int robco_smp_read_actlr(uint32_t *out);
int robco_smp_write_actlr(uint32_t value);
int robco_smp_read_cpuectlr(uint32_t out[2]);
int robco_smp_write_cpuectlr(uint32_t lo, uint32_t hi);

/* TTBR0 needs 16 KiB alignment. Own section, so --gc-sections drops it when unused. */
static uint32_t l1_table[4096] __attribute__((aligned(16384)));
static int mmu_ready;
static int smp_state = -1;

static inline uint32_t sctlr_read(void)
{
    uint32_t v;

    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

static inline uint32_t irq_mask_save(void)
{
    uint32_t cpsr;

    __asm__ volatile("mrs %0, cpsr\n cpsid if" : "=r"(cpsr) :: "memory");
    return cpsr;
}

static inline void irq_mask_restore(uint32_t cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

/* CP15 barriers: valid on ARMv7/v8 because start.S sets SCTLR.CP15BEN. */
static inline void dsb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static inline void isb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" :: "r"(0) : "memory");
}

static uint32_t cpu_part(void)
{
    return (board_get()->midr >> 4) & 0xfff;
}

int mmu_enabled(void)
{
    return (sctlr_read() & SCTLR_M) != 0;
}

static int cached(uint32_t d)
{
    return (d & 3) == SECT && (d & SECT_C) != 0;
}

/*
 * After descriptor writes. Table walks use TTBR0 walk attributes 0, so
 * they read memory, not the D-cache: clean the edited lines first. Then
 * drop stale TLB entries and branch predictions.
 */
static void table_sync(uint32_t first, uint32_t count)
{
    cache_clean_range(&l1_table[first], count * 4);
    dsb();
    if (mmu_enabled()) {
        __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory"); /* TLBIALL */
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0) : "memory"); /* BPIALL */
        dsb();
        isb();
    }
}

int mmu_map(uint32_t start, uint32_t size, unsigned attr)
{
    static const uint32_t desc[] = {
        [MMU_ATTR_NONE] = 0,
        [MMU_ATTR_RAM] = DESC_RAM,
        [MMU_ATTR_DEVICE] = DESC_DEVICE,
        [MMU_ATTR_FB] = DESC_FB,
        [MMU_ATTR_RO_CODE] = DESC_RO_CODE,
    };
    uint32_t first, last, i, d, s;
    int on, flushed = 0;

    if (size == 0 || attr >= sizeof(desc) / sizeof(desc[0]) || start + (size - 1) < start) {
        return MMU_ERR_ARG;
    }
    first = start >> 20;
    last = (start + (size - 1)) >> 20;

    s = irq_mask_save();
    on = mmu_enabled();
    for (i = first; i <= last; i++) {
        d = attr == MMU_ATTR_NONE ? 0 : (i << 20) | desc[attr];
        /*
         * A section that stops being cacheable may hold dirty lines.
         * Write them back while the old mapping still allows it.
         */
        if (on && cached(l1_table[i]) && !cached(d)) {
            cache_flush_range((const void *)(uintptr_t)(i << 20), MMU_SECTION_SIZE);
            flushed = 1;
        }
        l1_table[i] = d;
    }
    table_sync(first, last - first + 1);
    if (flushed) {
        /*
         * Cortex-A7/A53 may prefetch into the cache until the TLB
         * invalidate. Those lines are clean. Drop them through the new
         * mapping (ARMv7 line operations ignore the memory type). Sections
         * now unmapped are skipped: a line operation there would fault.
         * ARM1176 does not prefetch data, so it has nothing to drop.
         */
        for (i = first; i <= last; i++) {
            if (l1_table[i] != 0) {
                cache_flush_range((const void *)(uintptr_t)(i << 20), MMU_SECTION_SIZE);
            }
        }
    }
    irq_mask_restore(s);
    return 0;
}

int mmu_init(void)
{
    const robco_board_t *b = board_get();
    const fb_info_t *fb;
    uint32_t i;

    if (mmu_enabled()) {
        return MMU_ERR_ON;
    }
    if (b->periph_base == 0 || b->mem_size == 0) {
        return MMU_ERR_BOARD;
    }
    mmu_ready = 0;
    for (i = 0; i < 4096; i++) {
        l1_table[i] = 0;
    }
    mmu_map(b->mem_base, b->mem_size, MMU_ATTR_RAM);
    mmu_map(b->periph_base, PERIPH_SIZE, MMU_ATTR_DEVICE);
    if (b->soc == ROBCO_SOC_BCM2836 || b->soc == ROBCO_SOC_BCM2837) {
        mmu_map(LOCAL_PERIPH, MMU_SECTION_SIZE, MMU_ATTR_DEVICE);
    }
    /* fb_init() before mmu_init(): map the buffer now. After: fb_init() calls mmu_map(). */
    if (fb_get != 0) {
        fb = fb_get();
        if (fb != 0) {
            mmu_map((uint32_t)(uintptr_t)fb->addr, fb->size, MMU_ATTR_FB);
        }
    }
    mmu_ready = 1;
    return 0;
}

/*
 * Cortex-A7 (ACTLR bit 6) and Cortex-A53 (CPUECTLR.SMPEN) must have the
 * SMP bit set before the D-cache is on, or cache maintenance and
 * exclusives misbehave. The firmware usually sets it. The register may be
 * write-protected from the non-secure side, so write only if the bit is
 * clear, and through a probe that survives the UNDEFINED trap.
 */
static int set_smp_bit(uint32_t part)
{
    uint32_t v[2] = {0, 0};

    if (part == PART_A7) {
        if (robco_smp_read_actlr(&v[0]) != 0) {
            return 0;
        }
        if (!(v[0] & SMP_BIT)) {
            robco_smp_write_actlr(v[0] | SMP_BIT);
            if (robco_smp_read_actlr(&v[0]) != 0) {
                return 0;
            }
        }
        return (v[0] & SMP_BIT) != 0;
    }
    if (part == PART_A53) {
        if (robco_smp_read_cpuectlr(v) != 0) {
            return 0;
        }
        if (!(v[0] & SMP_BIT)) {
            robco_smp_write_cpuectlr(v[0] | SMP_BIT, v[1]);
            if (robco_smp_read_cpuectlr(v) != 0) {
                return 0;
            }
        }
        return (v[0] & SMP_BIT) != 0;
    }
    return 0;
}

void mmu_enable(void)
{
    uint32_t part = cpu_part();
    uint32_t sctlr, s;

    if (!mmu_ready || mmu_enabled()) {
        return;
    }
    s = irq_mask_save();
    smp_state = part == PART_ARM1176 ? -1 : set_smp_bit(part);

    /* Caches hold UNKNOWN data after reset: invalidate D, I, BP and TLB. */
    robco_cache_reset_all();

    __asm__ volatile(
        "mcr p15, 0, %0, c3, c0, 0\n"   /* DACR: domain 0 client, the rest no access */
        "mcr p15, 0, %1, c2, c0, 2\n"   /* TTBCR: N = 0, TTBR0 only */
        "mcr p15, 0, %2, c2, c0, 0\n"   /* TTBR0: walks non-cacheable */
        "mcr p15, 0, %1, c7, c5, 4\n"   /* ISB */
        "mcr p15, 0, %1, c8, c7, 0\n"   /* TLBIALL */
        "mcr p15, 0, %1, c7, c10, 4\n"  /* DSB */
        "mcr p15, 0, %1, c7, c5, 4\n"   /* ISB */
        :: "r"(1), "r"(0), "r"((uint32_t)(uintptr_t)l1_table) : "memory");

    sctlr = sctlr_read();
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_Z;
    /* No alignment checks (the C code may do unaligned byte-wise work), no TEX remap, no access flag. */
    sctlr &= ~(SCTLR_A | SCTLR_TRE | SCTLR_AFE);
    if (part == PART_ARM1176) {
        sctlr |= SCTLR_XP;
    }
    __asm__ volatile(
        "mcr p15, 0, %0, c1, c0, 0\n"
        "mcr p15, 0, %1, c7, c5, 4\n"   /* ISB: later fetches see the new SCTLR */
        :: "r"(sctlr), "r"(0) : "memory");
    irq_mask_restore(s);
}

int mmu_smp(void)
{
    if (cpu_part() == PART_ARM1176) {
        return -1;
    }
    return smp_state < 0 ? 0 : smp_state;
}
