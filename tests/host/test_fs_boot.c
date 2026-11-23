/* The boot volume rule: part_boot.c decisions and fs_mount_boot() on RAM cards. */
#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/fs.h>
#include <robco/part.h>

#include "rc_test.h"

#define BLOCKS   8192u
#define PART_LEN 1024u
#define NOT_FAT  0x83u

static uint64_t disk_words[BLOCKS * BLK_SECTOR_SIZE / 8];
static unsigned char *const disk = (unsigned char *)disk_words;
static blk_dev_t card;

static int sync_fails; /* nonzero: every sync of the card fails */

static int card_sync(blk_dev_t *dev)
{
    (void)dev;
    return sync_fails ? BLK_ERR_IO : 0;
}

static const char *const markers[] = { "/ROBCO/MFBOOT.SYS", "/ROBCO/MFBOOT.BAK" };

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* An empty card with an MBR signature and no entries. */
static void blank(void)
{
    memset(disk, 0, sizeof(disk_words));
    disk[510] = 0x55;
    disk[511] = 0xAA;
    CHECK_EQ(blk_ram_init(&card, disk, sizeof(disk_words), 0), 0);
    card.sync = card_sync;
    sync_fails = 0;
}

static uint32_t slot_lba(unsigned slot)
{
    return 2048u + slot * PART_LEN;
}

static void entry(unsigned slot, uint8_t type)
{
    unsigned char *e = disk + 446 + slot * 16;

    e[4] = type;
    put32(e + 8, slot_lba(slot));
    put32(e + 12, PART_LEN);
}

static void file(const char *path, uint32_t size)
{
    static const char data[16] = "MFBOOT";
    fs_file_t f;

    CHECK_EQ(fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC), FS_OK);
    if (size) {
        CHECK_EQ(fs_write(&f, data, size), (int)size);
    }
    CHECK_EQ(fs_close(&f), FS_OK);
}

/*
 * Formats the blocks of slot as FAT. what: 0 nothing more, 1 MFBOOT.SYS,
 * 2 MFBOOT.BAK, 3 a folder named MFBOOT.SYS, 4 an empty MFBOOT.SYS.
 */
static void fat(unsigned slot, int what)
{
    part_info_t p = { 0 };
    blk_dev_t d;

    p.dev = &card;
    p.start_lba = slot_lba(slot);
    p.block_count = PART_LEN;
    CHECK_EQ(part_open(&card, &p, &d), 0);
    CHECK_EQ(fs_mkfs(&d, NULL), FS_OK);
    CHECK_EQ(fs_mount("mk", &d), FS_OK);
    CHECK_EQ(fs_mkdir("mk:/ROBCO"), FS_OK);
    {
        char tag[] = "mk:/SLOT0";

        tag[8] = (char)('0' + slot); /* names the slot for check_mounted() */
        file(tag, 0);
    }
    if (what == 1) {
        file("mk:/ROBCO/MFBOOT.SYS", 16);
    } else if (what == 2) {
        file("mk:/ROBCO/MFBOOT.BAK", 16);
    } else if (what == 3) {
        CHECK_EQ(fs_mkdir("mk:/ROBCO/MFBOOT.SYS"), FS_OK);
    } else if (what == 4) {
        file("mk:/ROBCO/MFBOOT.SYS", 0);
    }
    CHECK_EQ(fs_unmount("mk"), FS_OK);
}

/* Fills the blocks of slot with a pattern that is no file system. */
static void junk(unsigned slot)
{
    memset(disk + (size_t)slot_lba(slot) * BLK_SECTOR_SIZE, 0xA5, PART_LEN * BLK_SECTOR_SIZE);
}

/*
 * The fs_mount_boot() invariant after rc: with PART_BOOT_OK or
 * PART_BOOT_ERR_RELEASE, "boot" is mounted and is the volume b describes;
 * with any other code nothing is mounted. Unmounts "boot" afterwards.
 */
static void check_mounted(int rc, const fs_boot_t *b)
{
    fs_stat_t st;
    char tag[] = "boot:/SLOT0";

    if (rc == PART_BOOT_OK || rc == PART_BOOT_ERR_RELEASE) {
        CHECK_EQ(fs_stat("boot:/", &st), FS_OK);
        if (b->slot != FS_BOOT_WHOLE) {
            tag[10] = (char)('0' + b->slot);
            CHECK_EQ(fs_stat(tag, &st), FS_OK);
            CHECK_EQ(b->start_lba, slot_lba(b->slot));
        }
        sync_fails = 0;
        CHECK_EQ(fs_unmount("boot"), FS_OK);
    }
    CHECK_EQ(fs_unmount("boot"), FS_ERR_NOVOL);
}

static int run(fs_boot_t *b, int with_markers)
{
    int rc = fs_mount_boot("boot", &card, with_markers ? markers : NULL, with_markers ? 2 : 0, b);

    check_mounted(rc, b);
    return rc;
}

TEST(class_and_order)
{
    part_info_t p[4];
    uint8_t order[PART_MAX];

    memset(p, 0, sizeof(p));
    p[0].type = PART_TYPE_EXTENDED;
    p[1].type = NOT_FAT;
    p[2].type = PART_TYPE_FAT32_LBA;
    p[3].type = PART_TYPE_GPT;
    CHECK_EQ(part_boot_class(&p[0]), PART_BOOT_NEVER);
    CHECK_EQ(part_boot_class(&p[1]), PART_BOOT_OTHER);
    CHECK_EQ(part_boot_class(&p[2]), PART_BOOT_FAT);
    CHECK_EQ(part_boot_class(&p[3]), PART_BOOT_NEVER);
    CHECK_EQ(part_boot_order(p, 4, order), 2u);
    CHECK_EQ(order[0], 2);
    CHECK_EQ(order[1], 1);

    p[0].type = PART_TYPE_EXT_LBA;
    CHECK_EQ(part_boot_class(&p[0]), PART_BOOT_NEVER);
    p[0].type = 0;
    p[0].superfloppy = 1;
    CHECK_EQ(part_boot_class(&p[0]), PART_BOOT_FAT);
    CHECK_EQ(part_boot_order(p, 4, order), 3u);
    CHECK_EQ(order[0], 0);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 1);
}

TEST(pick)
{
    const uint8_t a[] = { PART_PROBE_NOMOUNT, PART_PROBE_MOUNTED, PART_PROBE_MARKER };
    const uint8_t b[] = { PART_PROBE_NOMOUNT, PART_PROBE_MOUNTED, PART_PROBE_MOUNTED };
    const uint8_t c[] = { PART_PROBE_NOMOUNT, PART_PROBE_NOMOUNT };

    CHECK_EQ(part_boot_pick(a, 3), 2);
    CHECK_EQ(part_boot_pick(b, 3), 1);
    CHECK_EQ(part_boot_pick(c, 2), -1);
    CHECK_EQ(part_boot_pick(c, 0), -1);
}

TEST(failure_reasons)
{
    part_info_t p[2];

    memset(p, 0, sizeof(p));
    CHECK_EQ(part_boot_failure(-3, p), PART_BOOT_ERR_DEVICE);
    CHECK_EQ(part_boot_failure(0, p), PART_BOOT_ERR_NOFAT);
    p[0].type = PART_TYPE_GPT;
    CHECK_EQ(part_boot_failure(1, p), PART_BOOT_ERR_GPT);
    p[1].type = PART_TYPE_FAT16;
    CHECK_EQ(part_boot_failure(2, p), PART_BOOT_ERR_MOUNT);
    p[0].type = NOT_FAT;
    CHECK_EQ(part_boot_failure(1, p), PART_BOOT_ERR_NOFAT);
    CHECK_STR(part_boot_strerror(PART_BOOT_OK), "ok");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_DEVICE), "nodev");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_NOFAT), "nofat");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_GPT), "gpt");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_MOUNT), "mount");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_ARG), "arg");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_BUSY), "busy");
    CHECK_STR(part_boot_strerror(PART_BOOT_ERR_RELEASE), "release");
    CHECK_STR(part_boot_strerror(99), "unknown");
}

TEST(whole_card)
{
    fs_boot_t b;

    blank();
    CHECK_EQ(fs_mkfs(&card, NULL), FS_OK);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, FS_BOOT_WHOLE);
    CHECK_EQ(b.type, 0);
    CHECK_EQ(b.start_lba, 0);
    CHECK_EQ(b.block_count, BLOCKS);
    CHECK_EQ(b.marker, 0);
    CHECK_EQ(b.by_type, 1);
}

TEST(no_table_no_fat)
{
    fs_boot_t b;

    blank();
    disk[446] = 0x12; /* not a status byte: no table */
    CHECK_EQ(run(&b, 1), PART_BOOT_ERR_NOFAT);
    memset(disk, 0, BLK_SECTOR_SIZE);
    CHECK_EQ(run(&b, 0), PART_BOOT_ERR_NOFAT);
}

TEST(non_fat_first_then_fat)
{
    fs_boot_t b;

    blank();
    entry(0, NOT_FAT);
    junk(0);
    entry(1, PART_TYPE_FAT16);
    fat(1, 1);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
    CHECK_EQ(b.type, PART_TYPE_FAT16);
    CHECK_EQ(b.start_lba, slot_lba(1));
    CHECK_EQ(b.block_count, PART_LEN);
    CHECK_EQ(b.marker, 1);
    CHECK_EQ(b.by_type, 1);
    CHECK_EQ(run(&b, 0), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
}

TEST(empty_slot_then_fat)
{
    fs_boot_t b;

    blank();
    entry(2, PART_TYPE_FAT32_LBA);
    fat(2, 2);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 2);
    CHECK_EQ(b.marker, 1);
}

TEST(marker_only_on_second)
{
    fs_boot_t b;

    blank();
    entry(0, PART_TYPE_FAT16);
    fat(0, 0);
    entry(1, PART_TYPE_FAT16);
    fat(1, 2);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
    CHECK_EQ(b.marker, 1);
    /* Without markers the first volume that mounts wins. */
    CHECK_EQ(run(&b, 0), PART_BOOT_OK);
    CHECK_EQ(b.slot, 0);
    CHECK_EQ(b.marker, 0);
}

TEST(marker_on_both)
{
    fs_boot_t b;

    blank();
    entry(0, PART_TYPE_FAT16);
    fat(0, 1);
    entry(1, PART_TYPE_FAT16);
    fat(1, 1);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 0);
    CHECK_EQ(b.marker, 1);
}

TEST(folder_or_empty_file_is_no_marker)
{
    fs_boot_t b;
    int what;

    for (what = 3; what <= 4; what++) {
        blank();
        entry(0, PART_TYPE_FAT16);
        fat(0, what);
        entry(1, PART_TYPE_FAT16);
        fat(1, 1);
        CHECK_EQ(run(&b, 1), PART_BOOT_OK);
        CHECK_EQ(b.slot, 1);
        CHECK_EQ(b.marker, 1);
    }
}

TEST(no_marker_anywhere_takes_first_that_mounts)
{
    fs_boot_t b;

    blank();
    entry(0, PART_TYPE_FAT16);
    junk(0);
    entry(1, PART_TYPE_FAT16);
    fat(1, 0);
    entry(2, PART_TYPE_FAT16);
    fat(2, 0);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
    CHECK_EQ(b.marker, 0);
    CHECK_EQ(b.last_err, FS_ERR_NOFS);
    /* The volume stays mounted under the given name. */
    CHECK_EQ(fs_mount_boot("boot", &card, markers, 2, &b), PART_BOOT_OK);
    {
        fs_stat_t st;

        CHECK_EQ(fs_stat("boot:/ROBCO", &st), FS_OK);
        CHECK(st.is_dir);
    }
    CHECK_EQ(fs_unmount("boot"), FS_OK);
}

TEST(non_fat_only)
{
    fs_boot_t b;

    blank();
    entry(0, NOT_FAT);
    junk(0);
    entry(1, 0x07);
    junk(1);
    CHECK_EQ(run(&b, 1), PART_BOOT_ERR_NOFAT);
    CHECK_EQ(b.last_err, FS_ERR_NOFS);
}

TEST(fat_entries_none_mounts)
{
    fs_boot_t b;

    blank();
    entry(0, PART_TYPE_FAT32);
    junk(0);
    entry(1, NOT_FAT);
    junk(1);
    CHECK_EQ(run(&b, 1), PART_BOOT_ERR_MOUNT);
}

TEST(gpt)
{
    fs_boot_t b;

    blank();
    entry(0, PART_TYPE_GPT);
    fat(0, 1); /* even a FAT volume behind it is never mounted */
    CHECK_EQ(run(&b, 1), PART_BOOT_ERR_GPT);
}

TEST(extended_never_mounted)
{
    fs_boot_t b;

    blank();
    entry(0, PART_TYPE_EXTENDED);
    fat(0, 1);
    entry(1, PART_TYPE_EXT_LBA);
    fat(1, 1);
    CHECK_EQ(run(&b, 1), PART_BOOT_ERR_NOFAT);
}

TEST(wrong_type_byte_second_pass)
{
    fs_boot_t b;

    /* Only a wrong type byte: found in the second pass. */
    blank();
    entry(0, NOT_FAT);
    fat(0, 1);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 0);
    CHECK_EQ(b.type, NOT_FAT);
    CHECK_EQ(b.by_type, 0);

    /* A FAT type byte goes first, even in a later slot. */
    entry(1, PART_TYPE_FAT16);
    fat(1, 1);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
    CHECK_EQ(b.by_type, 1);

    /* A FAT entry without a marker loses to a wrong type byte with one. */
    fat(1, 0);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 0);
    CHECK_EQ(b.marker, 1);
    CHECK_EQ(run(&b, 0), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
}

TEST(busy_leaves_the_mounted_volume_alone)
{
    fs_boot_t b, other;
    fs_stat_t st;

    blank();
    entry(1, PART_TYPE_FAT16);
    fat(1, 1);
    CHECK_EQ(fs_mount_boot("boot", &card, markers, 2, &b), PART_BOOT_OK);
    CHECK_EQ(fs_mount_boot("boot", &card, markers, 2, &b), PART_BOOT_ERR_BUSY);
    CHECK_EQ(fs_mount_boot("boot", &card, NULL, 0, &other), PART_BOOT_ERR_BUSY);
    CHECK_EQ(fs_mount_boot("x", &card, NULL, 0, &b), PART_BOOT_ERR_BUSY);
    CHECK_EQ(b.slot, 1);
    CHECK_EQ(fs_stat("boot:/ROBCO/MFBOOT.SYS", &st), FS_OK);
    /* A second view of the same blocks is refused by fs_mount(). */
    CHECK_EQ(fs_mount_boot("x", &card, NULL, 0, &other), PART_BOOT_ERR_MOUNT);
    CHECK_EQ(other.last_err, FS_ERR_MOUNTED);
    CHECK_EQ(fs_unmount("boot"), FS_OK);
}

TEST(bad_device_and_args)
{
    fs_boot_t b;
    blk_dev_t odd;
    int rc;

    CHECK_EQ(rc = fs_mount_boot("boot", NULL, NULL, 0, &b), PART_BOOT_ERR_DEVICE);
    check_mounted(rc, &b);
    CHECK_EQ(rc = fs_mount_boot("boot", &card, NULL, 0, NULL), PART_BOOT_ERR_ARG);
    check_mounted(rc, &b);
    CHECK_EQ(rc = fs_mount_boot(NULL, &card, NULL, 0, &b), PART_BOOT_ERR_ARG);
    check_mounted(rc, &b);
    CHECK_EQ(rc = fs_mount_boot("boot", &card, NULL, 1, &b), PART_BOOT_ERR_ARG);
    check_mounted(rc, &b);
    blank();
    odd = card;
    odd.block_size = 4096;
    CHECK_EQ(rc = fs_mount_boot("boot", &odd, NULL, 0, &b), PART_BOOT_ERR_DEVICE);
    check_mounted(rc, &b);
    CHECK_EQ(b.last_err, BLK_ERR_PARAM);
}

/* Runs fs_mount_boot() with markers while every sync of the card fails. */
static int run_sync_fails(fs_boot_t *b)
{
    int rc;

    sync_fails = 1;
    rc = fs_mount_boot("boot", &card, markers, 2, b);
    sync_fails = 0;
    return rc;
}

TEST(unmount_fails_on_first_of_two)
{
    fs_boot_t b;
    int rc;

    blank();
    entry(0, PART_TYPE_FAT16);
    fat(0, 0);
    entry(1, PART_TYPE_FAT16);
    fat(1, 1);
    sync_fails = 1;
    rc = fs_mount_boot("boot", &card, markers, 2, &b);
    /* The probe stops on the volume that did not unmount. */
    CHECK_EQ(rc, PART_BOOT_ERR_RELEASE);
    CHECK_EQ(b.slot, 0);
    CHECK_EQ(b.marker, 0);
    CHECK_EQ(b.last_err, FS_ERR_IO);
    /* Reads still go to slot 0 while the flush keeps failing. */
    {
        fs_stat_t st;

        CHECK_EQ(fs_stat("boot:/SLOT0", &st), FS_OK);
        CHECK_EQ(fs_stat("boot:/ROBCO/MFBOOT.SYS", &st), FS_ERR_NOENT);
        CHECK_EQ(fs_unmount("boot"), FS_ERR_IO);
    }
    check_mounted(rc, &b);
    /* With a working device the marker volume wins again. */
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 1);
}

TEST(unmount_fails_on_fallback_candidate)
{
    fs_boot_t b;
    int rc;

    /* Slot 0 does not mount, slot 1 is the fallback, slot 2 holds the marker. */
    blank();
    entry(0, PART_TYPE_FAT16);
    junk(0);
    entry(1, PART_TYPE_FAT16);
    fat(1, 0);
    entry(2, PART_TYPE_FAT16);
    fat(2, 1);
    rc = run_sync_fails(&b);
    CHECK_EQ(rc, PART_BOOT_ERR_RELEASE);
    CHECK_EQ(b.slot, 1);
    CHECK_EQ(b.last_err, FS_ERR_IO);
    check_mounted(rc, &b);
    CHECK_EQ(run(&b, 1), PART_BOOT_OK);
    CHECK_EQ(b.slot, 2);
}

TEST(sync_failure_without_a_probe_unmount)
{
    fs_boot_t b;
    int rc;

    /* The first volume holds the marker: no unmount, so no failure. */
    blank();
    entry(0, PART_TYPE_FAT16);
    fat(0, 1);
    rc = run_sync_fails(&b);
    CHECK_EQ(rc, PART_BOOT_OK);
    CHECK_EQ(b.slot, 0);
    check_mounted(rc, &b);
    /* Nothing mounts: nothing to release. */
    junk(0);
    rc = run_sync_fails(&b);
    CHECK_EQ(rc, PART_BOOT_ERR_MOUNT);
    check_mounted(rc, &b);
}
