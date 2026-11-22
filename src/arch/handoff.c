#include <stdint.h>

#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/irq.h>

/* Weak, so an image without the interrupt driver does not link it in. */
#pragma weak irq_init

#define SCTLR_M (1u << 0)
#define SCTLR_C (1u << 2)
#define SCTLR_I (1u << 12)

void robco_handoff(uint32_t entry, const robco_bootinfo_t *bi, uint32_t fdt_or_atags)
{
    uint32_t mach = robco_boot_args()->r1;
    uint32_t sctlr, midr;

    __asm__ volatile("cpsid if" ::: "memory");

    /* Every source masked at the controller, the system timer included. */
    if (irq_init != 0) {
        irq_init();
    }

    /*
     * The library never turns on the MMU or the D-cache. If the image did,
     * turn both off. On ARM1176 also clean and invalidate the whole
     * D-cache first. ARMv7 has no whole-cache operation, so this function
     * does not clean there: the caller must. See the contract in boot.h.
     */
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    if (sctlr & (SCTLR_M | SCTLR_C)) {
        if (((midr >> 4) & 0xfff) == 0xb76) {
            __asm__ volatile("mcr p15, 0, %0, c7, c14, 0" :: "r"(0) : "memory");
        }
        sctlr &= ~(SCTLR_M | SCTLR_C);
        __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr) : "memory");
    }

    /* Drain writes, invalidate the I-cache and branch predictor, then sync. */
    __asm__ volatile(
        "mcr p15, 0, %0, c7, c10, 4\n"  /* DSB */
        "mcr p15, 0, %0, c7, c5, 0\n"   /* invalidate I-cache */
        "mcr p15, 0, %0, c7, c5, 6\n"   /* invalidate branch predictor */
        "mcr p15, 0, %0, c7, c10, 4\n"  /* DSB */
        "mcr p15, 0, %0, c7, c5, 4\n"   /* ISB */
        :: "r"(0) : "memory");

    /* SVC mode, IRQ and FIQ masked, then the jump with r0 to r3 set. */
    register uint32_t r0 __asm__("r0") = 0;
    register uint32_t r1 __asm__("r1") = mach;
    register uint32_t r2 __asm__("r2") = fdt_or_atags;
    register uint32_t r3 __asm__("r3") = (uint32_t)bi;
    __asm__ volatile(
        "cpsid if, #0x13\n"
        "bx %4\n"
        :: "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(entry) : "memory");
    __builtin_unreachable();
}
