#include <stdint.h>

#include "k_printf.h"

#ifndef SPIKE_STAGE
#error "SPIKE_STAGE must be set by the spike Makefile"
#endif
#define STR_(x) #x
#define STR(x) STR_(x)

void exc_report(uint32_t id, uint32_t pc);

static const char *const exc_names[] = {
    "reset", "undef", "svc", "prefetch_abort",
    "data_abort", "unused", "irq", "fiq",
};

/* Called from the vector stubs in start.S. Prints and returns to halt. */
void exc_report(uint32_t id, uint32_t pc)
{
    uint32_t dfsr;
    uint32_t dfar;
    uint32_t ifsr;

    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));

    k_printf("\n@RC stage=" STR(SPIKE_STAGE) " ev=exception name=%s pc=0x%08x dfsr=0x%08x dfar=0x%08x ifsr=0x%08x\n",
             id < 8 ? exc_names[id] : "?", (unsigned)pc, (unsigned)dfsr,
             (unsigned)dfar, (unsigned)ifsr);
}
