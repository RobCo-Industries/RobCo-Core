#include <stdint.h>

#include <robco/tick.h>

#include "rc_test.h"

TEST(period_range)
{
    CHECK_EQ(tick_period_us_for(0), 0);
    CHECK_EQ(tick_period_us_for(1), 1000000);
    CHECK_EQ(tick_period_us_for(3), 333333);
    CHECK_EQ(tick_period_us_for(7000), 142);
    CHECK_EQ(tick_period_us_for(10000), 100);
    CHECK_EQ(tick_period_us_for(10001), 0);
}

TEST(ms_100hz)
{
    uint32_t p = tick_period_us_for(100);

    CHECK_EQ(tick_to_ms(0, p), 0);
    CHECK_EQ(tick_to_ms(1, p), 10);
    CHECK_EQ(tick_to_ms(100, p), 1000);
}

TEST(ms_1000hz)
{
    uint32_t p = tick_period_us_for(1000);

    CHECK_EQ(tick_to_ms(1, p), 1);
    CHECK_EQ(tick_to_ms(123456, p), 123456);
}

TEST(ms_7000hz_uses_real_period)
{
    uint32_t p = tick_period_us_for(7000);

    /* 7000 ticks of 142 us take 994 ms, not 1000. */
    CHECK_EQ(tick_to_ms(7000, p), 994);
    CHECK_EQ(tick_to_ms(7000ull * 3600, p), 3578400);
}

TEST(ms_3hz)
{
    uint32_t p = tick_period_us_for(3);

    CHECK_EQ(tick_to_ms(1, p), 333);
    CHECK_EQ(tick_to_ms(3, p), 999);
    CHECK_EQ(tick_to_ms(3000, p), 999999);
}

TEST(ms_past_32bit_tick_wrap)
{
    uint32_t p = tick_period_us_for(10000);
    uint64_t ticks = (1ull << 32) + 10;

    CHECK_EQ(tick_to_ms(0xFFFFFFFFull, p), 429496729);
    CHECK_EQ(tick_to_ms(ticks, p), 429496730);
    /* At 1 Hz the product passes 2^32 and 2^44 ms still fits. */
    CHECK_EQ(tick_to_ms(ticks, tick_period_us_for(1)), ticks * 1000);
}
