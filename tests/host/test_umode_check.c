#include <robco/umode.h>

#include "rc_test.h"

#define TOP ((uintptr_t)-1)

static const umode_region_t two[] = {
    { 0x1000, 0x1000, 0 },                      /* read-only */
    { 0x4000, 0x1000, UMODE_REGION_WRITE },
};

/* Adjacent: 0x10000-0x10fff writable, 0x11000-0x11fff writable, 0x12000-0x12fff read-only. */
static const umode_region_t adj[] = {
    { 0x10000, 0x1000, UMODE_REGION_WRITE },
    { 0x11000, 0x1000, UMODE_REGION_WRITE },
    { 0x12000, 0x1000, 0 },
};

TEST(inside)
{
    CHECK(umode_regions_range_ok(two, 2, 0x1000, 0x1000, 0));
    CHECK(umode_regions_range_ok(two, 2, 0x1800, 0x10, 0));
    CHECK(umode_regions_range_ok(two, 2, 0x4000, 0x1000, 1));
    CHECK(umode_regions_range_ok(two, 2, 0x4fff, 1, 1));
}

TEST(outside_and_crossing_end)
{
    CHECK(!umode_regions_range_ok(two, 2, 0x0fff, 1, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x0fff, 2, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x1fff, 2, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x2000, 1, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x4000, 0x1001, 1));
    CHECK(!umode_regions_range_ok(two, 2, 0x1000, 0x3001, 0));  /* gap between regions */
}

TEST(read_only)
{
    CHECK(umode_regions_range_ok(two, 2, 0x1000, 4, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x1000, 4, 1));
    CHECK(umode_regions_range_ok(two, 2, 0x4000, 4, 0));    /* writable is readable */
}

TEST(empty_ranges)
{
    CHECK(umode_regions_range_ok(two, 2, 0x1000, 0, 0));
    CHECK(umode_regions_range_ok(two, 2, 0x2000, 0, 0));    /* at the end boundary */
    CHECK(!umode_regions_range_ok(two, 2, 0x2001, 0, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x0fff, 0, 0));
    CHECK(!umode_regions_range_ok(two, 2, 0x2000, 0, 1));   /* boundary of a read-only region */
    CHECK(umode_regions_range_ok(two, 2, 0x5000, 0, 1));
    CHECK(!umode_regions_range_ok(two, 2, 0, 0, 0));
    CHECK(!umode_regions_range_ok(NULL, 0, 0x1000, 4, 0));
    CHECK(!umode_regions_range_ok(two, 0, 0x1000, 4, 0));
}

TEST(adjacent)
{
    CHECK(umode_regions_range_ok(adj, 3, 0x10ff0, 0x20, 1));
    CHECK(umode_regions_range_ok(adj, 3, 0x10000, 0x2000, 1));
    CHECK(!umode_regions_range_ok(adj, 3, 0x10000, 0x2001, 1)); /* reaches the read-only one */
    CHECK(umode_regions_range_ok(adj, 3, 0x10000, 0x3000, 0));
    CHECK(!umode_regions_range_ok(adj, 3, 0x10000, 0x3001, 0));
    CHECK(umode_regions_range_ok(adj, 3, 0x11ff0, 0x20, 0));
}

TEST(wraparound)
{
    static const umode_region_t top[] = {
        { TOP - 0xfff, 0x1000, UMODE_REGION_WRITE },
        { 0, 0x1000, UMODE_REGION_WRITE },
    };

    CHECK(umode_regions_range_ok(top, 2, TOP - 0xfff, 0x1000, 1));
    CHECK(umode_regions_range_ok(top, 2, TOP, 1, 1));
    CHECK(!umode_regions_range_ok(top, 2, TOP, 2, 1));     /* would wrap to 0 */
    CHECK(!umode_regions_range_ok(top, 2, TOP - 0xf, (size_t)-1, 1));
    CHECK(!umode_regions_range_ok(two, 2, 0x1000, (size_t)-1, 0));
    CHECK(!umode_regions_range_ok(two, 2, TOP, 1, 0));
    CHECK(umode_regions_range_ok(top, 2, TOP, 0, 1));
}

TEST(strnlen)
{
    static char buf[64];
    umode_region_t r[2];
    uintptr_t b = (uintptr_t)buf;

    memset(buf, 'x', sizeof(buf));
    buf[10] = 0;
    r[0] = (umode_region_t){ b, 32, 0 };
    r[1] = (umode_region_t){ b + 32, 32, 0 };
    CHECK_EQ(umode_regions_strnlen(r, 1, b, 64), 10);
    CHECK_EQ(umode_regions_strnlen(r, 1, b + 10, 64), 0);
    CHECK_EQ(umode_regions_strnlen(r, 1, b, 10), (unsigned long long)-1); /* NUL not within max */
    CHECK_EQ(umode_regions_strnlen(r, 1, b, 11), 10);
    CHECK_EQ(umode_regions_strnlen(r, 1, b + 11, 64), (unsigned long long)-1); /* runs off region */
    buf[40] = 0;
    CHECK_EQ(umode_regions_strnlen(r, 2, b + 11, 64), 29);  /* crosses into the adjacent region */
    CHECK_EQ(umode_regions_strnlen(r, 2, b + 11, 0), (unsigned long long)-1);
    CHECK_EQ(umode_regions_strnlen(r, 2, b + 64, 8), (unsigned long long)-1);
    CHECK_EQ(umode_regions_strnlen(NULL, 0, b, 8), (unsigned long long)-1);
}
