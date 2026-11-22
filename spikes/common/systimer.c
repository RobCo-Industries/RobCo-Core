#include "systimer.h"
#include "mmio.h"

#define SYSTIMER_OFFSET 0x3000
#define SYSTIMER_CLO    0x04

static uintptr_t clo_addr;

void systimer_init(uintptr_t periph_base)
{
    clo_addr = periph_base + SYSTIMER_OFFSET + SYSTIMER_CLO;
}

uint32_t systimer_us(void)
{
    return clo_addr ? mmio_read(clo_addr) : 0;
}
