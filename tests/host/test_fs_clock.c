/* File time stamps from the clock: fs_set_clock() with a fake clock, and the fixed default. */
#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/datetime.h>
#include <robco/fs.h>

#include "rc_test.h"

#define DISK_SIZE (1024u * 1024u)

static uint64_t disk_words[DISK_SIZE / 8];
static blk_dev_t dev;
static datetime_t fake_now;
static int fake_set;
static unsigned fake_calls;

static int fake_clock(datetime_t *out)
{
    fake_calls++;
    if (!fake_set) {
        return -2;
    }
    *out = fake_now;
    return 0;
}

static void setup(void)
{
    fs_unmount("c");
    memset(disk_words, 0, sizeof(disk_words));
    CHECK_EQ(blk_ram_init(&dev, disk_words, DISK_SIZE, 0), 0);
    CHECK_EQ(fs_mkfs(&dev, NULL), FS_OK);
    CHECK_EQ(fs_mount("c", &dev), FS_OK);
}

static void touch(const char *path)
{
    fs_file_t f;

    CHECK_EQ(fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC), FS_OK);
    CHECK_EQ(fs_write(&f, "x", 1), 1);
    CHECK_EQ(fs_close(&f), FS_OK);
}

static void stamp(const char *path, datetime_t *out)
{
    fs_stat_t st;

    CHECK_EQ(fs_stat(path, &st), FS_OK);
    CHECK_EQ(dt_from_fat(st.date, st.time, out), 0);
}

TEST(clock_set_stamps_new_and_modified_files)
{
    datetime_t got;

    setup();
    fake_now = (datetime_t){ 2031, 7, 14, 9, 26, 53, 0 };
    fake_set = 1;
    fake_calls = 0;
    fs_set_clock(fake_clock);
    touch("c:/NEW.TXT");
    CHECK(fake_calls > 0);
    stamp("c:/NEW.TXT", &got);
    CHECK_EQ(got.year, 2031);
    CHECK_EQ(got.month, 7);
    CHECK_EQ(got.day, 14);
    CHECK_EQ(got.hour, 9);
    CHECK_EQ(got.minute, 26);
    CHECK_EQ(got.second, 52); /* FAT keeps 2-second steps */

    fake_now = (datetime_t){ 2040, 2, 29, 23, 59, 58, 0 };
    touch("c:/NEW.TXT");
    stamp("c:/NEW.TXT", &got);
    CHECK_EQ(got.year, 2040);
    CHECK_EQ(got.month, 2);
    CHECK_EQ(got.day, 29);
    CHECK_EQ(got.second, 58);
    CHECK_EQ(fs_write_file_atomic("c:/ATOM.SAV", "abc", 3), FS_OK);
    stamp("c:/ATOM.SAV", &got);
    CHECK_EQ(got.year, 2040);
    fs_set_clock(NULL);
    fs_unmount("c");
}

TEST(unset_clock_gives_fixed_time)
{
    datetime_t got;

    setup();
    fake_set = 0;
    fake_calls = 0;
    fs_set_clock(fake_clock);
    touch("c:/OLD.TXT");
    CHECK(fake_calls > 0);
    stamp("c:/OLD.TXT", &got);
    CHECK_EQ(got.year, 2026);
    CHECK_EQ(got.month, 1);
    CHECK_EQ(got.day, 1);
    CHECK_EQ(got.hour, 0);
    CHECK_EQ(got.minute, 0);

    /* Back to the default: no clock linked in host tests. */
    fs_set_clock(NULL);
    fake_calls = 0;
    touch("c:/DEF.TXT");
    CHECK_EQ(fake_calls, 0);
    stamp("c:/DEF.TXT", &got);
    CHECK_EQ(got.year, 2026);
    CHECK_EQ(fs_time_hook(), (uint32_t)((2026u - 1980u) << 25 | 1u << 21 | 1u << 16));
    fs_unmount("c");
}

TEST(invalid_clock_value_gives_fixed_time)
{
    fake_now = (datetime_t){ 2031, 13, 1, 0, 0, 0, 0 };
    fake_set = 1;
    fs_set_clock(fake_clock);
    CHECK_EQ(fs_time_hook(), (uint32_t)((2026u - 1980u) << 25 | 1u << 21 | 1u << 16));
    fs_set_clock(NULL);
}
