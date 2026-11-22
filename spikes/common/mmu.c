#include "mmu.h"

#define PART_ARM1176 0xb76
#define PART_A7      0xc07
#define PART_A53     0xd03

/* Short-descriptor 1 MiB section fields. */
#define SECT         0x2u
#define SECT_B       (1u << 2)
#define SECT_C       (1u << 3)
#define SECT_XN      (1u << 4)
#define SECT_AP_RW   (3u << 10)          /* AP[1:0] = 11, AP[2] = 0: full access */
#define SECT_TEX(x)  ((uint32_t)(x) << 12)

/* Normal, inner and outer write-back write-allocate (TEX 001, C 1, B 1). */
#define SECT_NORMAL  (SECT | SECT_AP_RW | SECT_TEX(1) | SECT_C | SECT_B)
/* Shareable device (TEX 000, C 0, B 1), execute-never. */
#define SECT_DEVICE  (SECT | SECT_AP_RW | SECT_B | SECT_XN)

#define SCTLR_M      (1u << 0)
#define SCTLR_A      (1u << 1)
#define SCTLR_C      (1u << 2)
#define SCTLR_Z      (1u << 11)
#define SCTLR_I      (1u << 12)
#define SCTLR_XP     (1u << 23)          /* ARMv6: use v6 descriptors (XN) */
#define SCTLR_TRE    (1u << 28)
#define SCTLR_AFE    (1u << 29)

#define SMP_BIT      (1u << 6)           /* ACTLR.SMP (A7), CPUECTLR.SMPEN (A53) */

void mmu_v6_inval_all(void);
uint32_t mmu_v6_enable(uint32_t ttbr0, uint32_t set, uint32_t clear);
void mmu_v6_clean_range(uintptr_t start, uintptr_t end);
void mmu_v7_inval_all(void);
uint32_t mmu_v7_enable(uint32_t ttbr0, uint32_t set, uint32_t clear);
void mmu_v7_clean_range(uintptr_t start, uintptr_t end);
int mmu_v7_try_read_actlr(uint32_t *out);
int mmu_v7_try_write_actlr(uint32_t value);
int mmu_v7_try_read_cpuectlr(uint32_t out[2]);
int mmu_v7_try_write_cpuectlr(uint32_t lo, uint32_t hi);

static uint32_t l1_table[4096] __attribute__((aligned(16384)));
static int mmu_kind; /* 0: off, 6: ARMv6, 7: ARMv7/v8 */

void mmu_build_identity(uintptr_t periph_base, int local_periph,
                        uint32_t fb_addr, uint32_t fb_size)
{
    uint32_t i;
    uint32_t addr;
    uint32_t periph_mb = (uint32_t)periph_base >> 20;
    uint32_t fb_first = fb_addr >> 20;
    uint32_t fb_last = (fb_addr + fb_size - 1) >> 20;

    for (i = 0; i < 4096; i++) {
        addr = i << 20;
        if (i < periph_mb) {
            l1_table[i] = addr | SECT_NORMAL;
        } else if (i < periph_mb + 16) {
            l1_table[i] = addr | SECT_DEVICE;
        } else if (local_periph && i == (0x40000000u >> 20)) {
            l1_table[i] = addr | SECT_DEVICE;
        } else {
            l1_table[i] = 0;
        }
    }
    if (fb_size != 0) {
        for (i = fb_first; i <= fb_last && i < 4096; i++) {
            l1_table[i] = (i << 20) | SECT_NORMAL;
        }
    }
}

static void set_smp_bit(uint32_t part, struct mmu_status *st)
{
    uint32_t v[2] = {0, 0};

    st->smp_reg = "none";
    st->smp_write = "none";
    st->smp_before = 0;
    st->smp_after = 0;

    if (part == PART_A7) {
        st->smp_reg = "actlr";
        if (mmu_v7_try_read_actlr(&v[0]) != 0) {
            st->smp_before = st->smp_after = -1;
            st->smp_write = "trapped";
            return;
        }
        st->smp_before = (v[0] & SMP_BIT) != 0;
        if (st->smp_before) {
            st->smp_write = "skipped";
        } else if (mmu_v7_try_write_actlr(v[0] | SMP_BIT) != 0) {
            st->smp_write = "trapped";
        } else {
            st->smp_write = "ok";
        }
        mmu_v7_try_read_actlr(&v[0]);
        st->smp_after = (v[0] & SMP_BIT) != 0;
    } else if (part == PART_A53) {
        st->smp_reg = "cpuectlr";
        if (mmu_v7_try_read_cpuectlr(v) != 0) {
            st->smp_before = st->smp_after = -1;
            st->smp_write = "trapped";
            return;
        }
        st->smp_before = (v[0] & SMP_BIT) != 0;
        if (st->smp_before) {
            st->smp_write = "skipped";
        } else if (mmu_v7_try_write_cpuectlr(v[0] | SMP_BIT, v[1]) != 0) {
            st->smp_write = "trapped";
        } else {
            st->smp_write = "ok";
        }
        mmu_v7_try_read_cpuectlr(v);
        st->smp_after = (v[0] & SMP_BIT) != 0;
    }
    if (st->smp_write[0] == 'o' && !st->smp_after) {
        st->smp_write = "ignored";
    }
}

void mmu_enable(uint32_t part, struct mmu_status *st)
{
    uint32_t ttbr0 = (uint32_t)(uintptr_t)l1_table;
    uint32_t set = SCTLR_M | SCTLR_C | SCTLR_Z | SCTLR_I;

    if (part == PART_ARM1176) {
        st->smp_reg = "none";
        st->smp_write = "none";
        st->smp_before = st->smp_after = 0;
        mmu_v6_inval_all();
        st->sctlr = mmu_v6_enable(ttbr0, set | SCTLR_XP, SCTLR_A);
        mmu_kind = 6;
    } else {
        set_smp_bit(part, st);
        mmu_v7_inval_all();
        st->sctlr = mmu_v7_enable(ttbr0, set, SCTLR_A | SCTLR_TRE | SCTLR_AFE);
        mmu_kind = 7;
    }
}

void mmu_clean_dcache_range(uintptr_t start, uintptr_t end)
{
    if (mmu_kind == 6) {
        mmu_v6_clean_range(start, end);
    } else if (mmu_kind == 7) {
        mmu_v7_clean_range(start, end);
    }
}
