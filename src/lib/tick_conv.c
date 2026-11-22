#include <stdint.h>

#include <robco/tick.h>

uint32_t tick_period_us_for(uint32_t hz)
{
    if (hz < TICK_HZ_MIN || hz > TICK_HZ_MAX) {
        return 0;
    }
    return 1000000u / hz;
}

uint64_t tick_to_ms(uint64_t ticks, uint32_t period_us)
{
    /* ticks below 2^44 keep the product below 2^64: over 55 years at 10 kHz. */
    return ticks * period_us / 1000u;
}
