#include "timer.h"
#include "mmio.h"

#define TIMER_OFFSET 0x3000
#define TIMER_CLO    0x04

static uintptr_t timer_base;

void timer_init(uintptr_t periph_base)
{
    timer_base = periph_base + TIMER_OFFSET;
}

uint32_t timer_now_us(void)
{
    return mmio_read(timer_base + TIMER_CLO);
}

int timer_expired(uint32_t start, uint32_t us)
{
    return (uint32_t)(timer_now_us() - start) >= us;
}

void timer_delay_us(uint32_t us)
{
    uint32_t start = timer_now_us();

    while (!timer_expired(start, us)) {
    }
}
