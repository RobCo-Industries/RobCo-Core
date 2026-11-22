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

/* umode_task_check(): a board with 64 MiB of RAM at 0, MMU on. */
#define MB      0x100000u
#define SEC     ((size_t)MB)

static const umode_forbidden_t kmem[] = {
    { 0x00004000, 0x4000, UMODE_ERR_TABLE },
    { 0x00008000, 0x40, UMODE_ERR_VECTORS },
    { 0x00008000, 0x80000, UMODE_ERR_KERNEL },
    { 0x03f00000, 0x01000000, UMODE_ERR_PERIPH },
};

static umode_limits_t lim_mmu(void)
{
    return (umode_limits_t){ 0, 0x04000000, SEC, kmem, 4 };
}

static umode_limits_t lim_flat(void)
{
    return (umode_limits_t){ 0, 0x04000000, 0, kmem, 4 };
}

/* A valid two-region task: code at 4 MiB read-only, data at 16 MiB writable. */
static umode_region_t reg[UMODE_MAX_REGIONS + 1];

static umode_task_t task2(void)
{
    reg[0] = (umode_region_t){ 4 * MB, MB, 0 };
    reg[1] = (umode_region_t){ 16 * MB, MB, UMODE_REGION_WRITE };
    return (umode_task_t){ .entry = 4 * MB, .sp = 17 * MB, .stub = 16 * MB,
                           .regions = reg, .region_count = 2 };
}

TEST(task_accepts_valid)
{
    umode_limits_t m = lim_mmu(), f = lim_flat();
    umode_task_t t = task2();

    CHECK_EQ(umode_task_check(&t, &m), 0);
    CHECK_EQ(umode_task_check(&t, &f), 0);
    t.sp = 16 * MB + 0x800;                         /* inside, not at the top */
    CHECK_EQ(umode_task_check(&t, &m), 0);
    t.sp = 16 * MB + 8;                             /* lowest legal sp */
    CHECK_EQ(umode_task_check(&t, &m), 0);
}

TEST(task_accepts_adjacent_sections)
{
    umode_limits_t m = lim_mmu();
    umode_task_t t = task2();

    /* Read-only 4 MiB and writable 5 MiB: adjacent, different sections. */
    reg[1] = (umode_region_t){ 5 * MB, MB, UMODE_REGION_WRITE };
    t.sp = 6 * MB;
    t.stub = 5 * MB;
    CHECK_EQ(umode_task_check(&t, &m), 0);
    /* A buffer across the boundary is readable, not writable. */
    CHECK(umode_regions_range_ok(reg, 2, 5 * MB - 4, 8, 0));
    CHECK(!umode_regions_range_ok(reg, 2, 5 * MB - 4, 8, 1));
    /* The stub may straddle two adjacent writable regions. */
    reg[0].flags = UMODE_REGION_WRITE;
    t.stub = 5 * MB - 4;
    CHECK_EQ(umode_task_check(&t, &m), 0);
}

TEST(task_rejects_args)
{
    umode_limits_t m = lim_mmu();
    umode_task_t t = task2();
    unsigned i;

    CHECK_EQ(umode_task_check(rc_launder(NULL), &m), UMODE_ERR_ARG);
    CHECK_EQ(umode_task_check(&t, rc_launder(NULL)), UMODE_ERR_ARG);
    t.region_count = 0;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ARG);
    t = task2();
    t.regions = rc_launder(NULL);
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ARG);
    for (i = 0; i <= UMODE_MAX_REGIONS; i++) {
        reg[i] = (umode_region_t){ (8 + i) * MB, MB, UMODE_REGION_WRITE };
    }
    t = (umode_task_t){ .entry = 8 * MB, .sp = 9 * MB, .stub = 8 * MB + 8,
                        .regions = reg, .region_count = UMODE_MAX_REGIONS };
    CHECK_EQ(umode_task_check(&t, &m), 0);
    t.region_count = UMODE_MAX_REGIONS + 1;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ARG);
    t.region_count = (unsigned)-1;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ARG);
    t = task2();
    reg[1].flags = 2;                               /* unknown flag bit */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ARG);
    m.forbidden = rc_launder(NULL);
    t = task2();
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ARG);
}

TEST(task_rejects_size_and_wrap)
{
    umode_limits_t f = lim_flat();
    umode_task_t t = task2();

    reg[1].size = 0;
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_RANGE);
    reg[1] = (umode_region_t){ TOP - 0xfff, 0x1001, UMODE_REGION_WRITE };
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_RANGE);
    reg[1] = (umode_region_t){ 16 * MB, (size_t)-1, UMODE_REGION_WRITE };
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_RANGE);
    /* Ends exactly at the top: no wrap, but outside RAM. */
    reg[1] = (umode_region_t){ TOP - 0xfff, 0x1000, UMODE_REGION_WRITE };
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_RAM);
}

TEST(task_rejects_overlap)
{
    umode_limits_t m = lim_mmu(), f = lim_flat();
    umode_task_t t = task2();

    /* Read-only then writable over the same megabyte, and the other order. */
    reg[1] = (umode_region_t){ 4 * MB, MB, UMODE_REGION_WRITE };
    t.sp = 5 * MB;
    t.stub = 4 * MB + 0x100;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_OVERLAP);
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_OVERLAP);
    reg[0].flags = UMODE_REGION_WRITE;
    reg[1].flags = 0;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_OVERLAP);
    /* One byte in common, flat. */
    reg[0] = (umode_region_t){ 4 * MB, 0x1000, 0 };
    reg[1] = (umode_region_t){ 4 * MB + 0xfff, 0x1000, UMODE_REGION_WRITE };
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_OVERLAP);
    /* One inside the other. */
    reg[0] = (umode_region_t){ 4 * MB, 4 * MB, 0 };
    reg[1] = (umode_region_t){ 5 * MB, MB, UMODE_REGION_WRITE };
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_OVERLAP);
    /* Overlap between the last two of three. */
    reg[0] = (umode_region_t){ 8 * MB, MB, UMODE_REGION_WRITE };
    reg[1] = (umode_region_t){ 4 * MB, 2 * MB, 0 };
    reg[2] = (umode_region_t){ 5 * MB, 2 * MB, UMODE_REGION_WRITE };
    t.region_count = 3;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_OVERLAP);
}

TEST(task_rejects_shared_section)
{
    umode_limits_t m = lim_mmu(), f = lim_flat();
    umode_task_t t = task2();

    /* Disjoint bytes in one megabyte: one descriptor for both. */
    reg[0] = (umode_region_t){ 4 * MB, 0x1000, 0 };
    reg[1] = (umode_region_t){ 4 * MB + 0x1000, 0x1000, UMODE_REGION_WRITE };
    t.sp = 4 * MB + 0x2000;
    t.stub = 4 * MB + 0x1000;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_SECTION);
    CHECK_EQ(umode_task_check(&t, &f), 0);          /* fine without the MMU */
}

TEST(task_rejects_align)
{
    umode_limits_t m = lim_mmu(), f = lim_flat();
    umode_task_t t = task2();

    reg[1] = (umode_region_t){ 16 * MB + 0x1000, MB, UMODE_REGION_WRITE };
    t.stub = 16 * MB + 0x1000;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ALIGN);
    CHECK_EQ(umode_task_check(&t, &f), 0);
    t = task2();
    reg[1].size = MB + 8;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ALIGN);
    reg[1].size = MB / 2;
    t.sp = 16 * MB + MB / 2;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ALIGN);
}

TEST(task_rejects_kernel_memory)
{
    umode_limits_t m = lim_mmu(), f = lim_flat();
    umode_task_t t = task2();

    reg[0] = (umode_region_t){ 0, MB, 0 };          /* table, vectors and image */
    t.entry = 0x00200000;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_TABLE);
    reg[0] = (umode_region_t){ 0x4000, 0x8000, 0 };
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_TABLE);
    reg[0] = (umode_region_t){ 0x8000, 4, 0 };
    t.entry = 0x8000;
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_VECTORS);
    reg[0] = (umode_region_t){ 0x87ffc, 4, 0 };     /* last word of the image */
    t.entry = 0x87ffc;
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_KERNEL);
    reg[0] = (umode_region_t){ 0x88000, 4, 0 };     /* just past it */
    t.entry = 0x88000;
    CHECK_EQ(umode_task_check(&t, &f), 0);
    reg[0] = (umode_region_t){ 0x3ff0000, 0x10000, 0 };     /* RAM end runs into periph */
    t.entry = 0x3ff0000;
    CHECK_EQ(umode_task_check(&t, &f), UMODE_ERR_PERIPH);
    reg[0] = (umode_region_t){ 0x3f00000, MB, 0 };
    t.entry = 0x3f00000;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_PERIPH);
}

TEST(task_rejects_outside_ram)
{
    umode_limits_t m = lim_mmu();
    umode_task_t t = task2();

    m.forbidden_count = 0;
    reg[0] = (umode_region_t){ 0x04000000, MB, 0 };         /* first byte past RAM */
    t.entry = 0x04000000;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_RAM);
    reg[0] = (umode_region_t){ 0x03f00000, 2 * MB, 0 };     /* crosses the end */
    t.entry = 0x03f00000;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_RAM);
    reg[0] = (umode_region_t){ 0x03f00000, MB, 0 };         /* last megabyte: fine */
    CHECK_EQ(umode_task_check(&t, &m), 0);
    m.ram_start = 8 * MB;                                   /* below RAM */
    m.ram_size = 0x04000000;
    t = task2();
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_RAM);
    m.ram_size = 0;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_RAM);
}

TEST(task_rejects_entry_sp_stub)
{
    umode_limits_t m = lim_mmu();
    umode_task_t t = task2();

    t.entry = 4 * MB + 2;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ENTRY);
    t.entry = 6 * MB;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_ENTRY);
    t.entry = 5 * MB - 4;                           /* last word: fine */
    CHECK_EQ(umode_task_check(&t, &m), 0);
    t.entry = 16 * MB + 0x100;                      /* writable regions run too */
    CHECK_EQ(umode_task_check(&t, &m), 0);

    t = task2();
    t.sp = 17 * MB - 4;                             /* not 8-aligned */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_SP);
    t.sp = 5 * MB;                                  /* top of the read-only region */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_SP);
    t.sp = 16 * MB;                                 /* bottom: nothing below it */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_SP);
    t.sp = 17 * MB + 8;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_SP);
    t.sp = 0;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_SP);

    t = task2();
    t.stub = 16 * MB + 2;
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_STUB);
    t.stub = 4 * MB + 0x100;                        /* read-only */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_STUB);
    t.stub = 17 * MB - 4;                           /* runs off the end */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_STUB);
    t.stub = 17 * MB - 8;                           /* last 8 bytes: fine */
    CHECK_EQ(umode_task_check(&t, &m), 0);
    t.entry = 16 * MB + 0x100;
    t.stub = 16 * MB + 0x100;                       /* on the entry */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_STUB);
    t.stub = 16 * MB + 0xfc;                        /* second word on the entry */
    CHECK_EQ(umode_task_check(&t, &m), UMODE_ERR_STUB);
    t.stub = 16 * MB + 0xf8;                        /* just before it */
    CHECK_EQ(umode_task_check(&t, &m), 0);
    t.stub = 16 * MB + 0x104;                       /* just after it */
    CHECK_EQ(umode_task_check(&t, &m), 0);
}

/*
 * For every accepted layout, the section map the MMU step builds (one
 * region per section) and umode_regions_range_ok() give the same answer
 * for every section, read and write.
 */
TEST(task_mmu_view_matches_checks)
{
    static const unsigned layouts[][3] = {
        /* start MiB, size MiB, flags */
        { 4, 1, 0 }, { 5, 1, 1 }, { 7, 3, 1 },
    };
    umode_limits_t m = lim_mmu();
    umode_task_t t = task2();
    unsigned i, s, w;

    for (i = 0; i < 3; i++) {
        reg[i] = (umode_region_t){ layouts[i][0] * MB, layouts[i][1] * MB, layouts[i][2] };
    }
    t.region_count = 3;
    t.sp = 6 * MB;
    t.stub = 5 * MB;
    CHECK_EQ(umode_task_check(&t, &m), 0);
    for (s = 0; s < 64; s++) {
        int perm = -1;  /* -1 unmapped, 0 read-only, 1 writable */

        for (i = 0; i < 3; i++) {
            if (s >= reg[i].start / MB && s < (reg[i].start + reg[i].size) / MB) {
                CHECK(perm == -1);                  /* one descriptor, one owner */
                perm = (int)(reg[i].flags & UMODE_REGION_WRITE);
            }
        }
        for (w = 0; w < 2; w++) {
            int want = perm >= (int)w;

            CHECK_EQ(umode_regions_range_ok(reg, 3, s * MB, MB, (int)w), want);
            CHECK_EQ(umode_regions_range_ok(reg, 3, s * MB + MB - 1, 1, (int)w), want);
        }
    }
}
