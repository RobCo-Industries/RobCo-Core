#include <stdint.h>

#include <robco/boot.h>
#include <robco/exc.h>
#include <robco/klibc.h>

static const char *const exc_names[] = {
    "reset", "undef", "svc", "prefetch_abort",
    "data_abort", "unused", "irq", "fiq",
};

void robco_exc_report(uint32_t type, uint32_t pc, uint32_t lr, uint32_t spsr)
{
    uint32_t dfsr;
    uint32_t dfar;

    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));

    k_printf("\n@RC stage=%s ev=exception type=%s pc=0x%08x lr=0x%08x spsr=0x%08x "
             "dfsr=0x%08x dfar=0x%08x\n",
             ROBCO_STAGE_NAME, type < 8 ? exc_names[type] : "?", (unsigned)pc,
             (unsigned)lr, (unsigned)spsr, (unsigned)dfsr, (unsigned)dfar);
    robco_halt();
}
