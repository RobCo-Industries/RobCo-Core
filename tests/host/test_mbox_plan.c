/* Mailbox cache plan: bounce or direct, and which bytes get maintenance. */
#include <stdint.h>

#include <robco/mbox.h>

#include "rc_test.h"

#define BOUNCE      0x10000u
#define BOUNCE_SIZE 256u

static int plan(uintptr_t buf, uint32_t len, uint32_t line, mbox_plan_t *p)
{
    return mbox_plan(buf, len, line, BOUNCE, BOUNCE_SIZE, p);
}

static void check_bounce(uintptr_t buf, uint32_t len, uint32_t line, uint32_t want_len)
{
    mbox_plan_t p;

    CHECK_EQ(plan(buf, len, line, &p), MBOX_PLAN_BOUNCE);
    CHECK_EQ(p.mode, MBOX_PLAN_BOUNCE);
    CHECK_EQ(p.start, BOUNCE);
    CHECK_EQ(p.len, want_len);
}

static void check_direct(uintptr_t buf, uint32_t len, uint32_t line)
{
    mbox_plan_t p;

    CHECK_EQ(plan(buf, len, line, &p), MBOX_PLAN_DIRECT);
    CHECK_EQ(p.start, buf);
    CHECK_EQ(p.len, len);
}

static void check_reject(uintptr_t buf, uint32_t len, uint32_t line)
{
    mbox_plan_t p;

    CHECK_EQ(plan(buf, len, line, &p), MBOX_PLAN_REJECT);
    CHECK_EQ(p.start, 0);
    CHECK_EQ(p.len, 0);
}

TEST(unaligned_start)
{
    check_bounce(0x8010, 64, 64, 64);
    check_bounce(0x8010, 32, 32, 32);
    check_bounce(0x8030, 128, 64, 128);
}

TEST(unaligned_end)
{
    check_bounce(0x8000, 32, 64, 64);
    check_bounce(0x8000, 48, 32, 64);
    check_bounce(0x8000, 144, 64, 192);
}

TEST(unaligned_both)
{
    check_bounce(0x8010, 40, 64, 64);
    check_bounce(0x8010, 40, 32, 64);
    check_bounce(0x8070, 200, 64, 256);
}

TEST(already_isolated)
{
    check_direct(0x8000, 64, 64);
    check_direct(0x8040, 192, 64);
    check_direct(0x8020, 32, 32);
    check_direct(0x8000, 512, 64);
}

TEST(larger_than_bounce)
{
    check_reject(0x8010, BOUNCE_SIZE + 16, 64);
    check_reject(0x8000, BOUNCE_SIZE + 4, 64);
    check_reject(0x8000, UINT32_MAX, 64);
    /* Isolated messages need no bounce, whatever their size. */
    check_direct(0x8000, BOUNCE_SIZE + 64, 64);
    check_bounce(0x8010, BOUNCE_SIZE, 64, BOUNCE_SIZE);
}

TEST(zero_length)
{
    check_reject(0x8000, 0, 64);
    check_reject(0x8010, 0, 32);
}

TEST(lengths_around_32)
{
    check_bounce(0x8000, 28, 32, 32);
    check_direct(0x8000, 32, 32);
    check_bounce(0x8000, 36, 32, 64);
    check_bounce(0x8000, 28, 64, 64);
    check_bounce(0x8000, 32, 64, 64);
    check_bounce(0x8000, 36, 64, 64);
}

TEST(lengths_around_64)
{
    check_bounce(0x8000, 60, 64, 64);
    check_direct(0x8000, 64, 64);
    check_bounce(0x8000, 68, 64, 128);
    check_bounce(0x8000, 124, 64, 128);
    check_direct(0x8000, 128, 64);
    check_bounce(0x8000, 132, 64, 192);
    check_direct(0x8000, 64, 32);
    check_direct(0x8000, 96, 32);
}

TEST(bad_line_or_bounce)
{
    mbox_plan_t p;

    check_reject(0x8000, 64, 0);
    check_reject(0x8000, 64, 48);
    CHECK_EQ(mbox_plan(0x8010, 32, 64, BOUNCE + 16, BOUNCE_SIZE, &p), MBOX_PLAN_REJECT);
    CHECK_EQ(mbox_plan(0x8010, 32, 64, BOUNCE, 0, &p), MBOX_PLAN_REJECT);
    CHECK_EQ(mbox_plan(0x8010, 32, 64, BOUNCE, 96, &p), MBOX_PLAN_REJECT);
}

TEST(no_wrap)
{
    mbox_plan_t p;

    CHECK_EQ(mbox_plan(UINTPTR_MAX - 63, 64, 64, BOUNCE, BOUNCE_SIZE, &p), MBOX_PLAN_DIRECT);
    CHECK_EQ(mbox_plan(UINTPTR_MAX - 63, 128, 64, BOUNCE, BOUNCE_SIZE, &p), MBOX_PLAN_BOUNCE);
}
