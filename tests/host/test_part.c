#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/fs.h>
#include <robco/part.h>

#include "blk_file.h"
#include "rc_test.h"

#define BLOCKS 4096u

static uint64_t disk_words[BLOCKS * BLK_SECTOR_SIZE / 8];
static unsigned char *const disk = (unsigned char *)disk_words;

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void entry(unsigned slot, uint8_t status, uint8_t type, uint32_t start, uint32_t count)
{
    unsigned char *e = disk + 446 + slot * 16;

    e[0] = status;
    e[4] = type;
    put32(e + 8, start);
    put32(e + 12, count);
}

static blk_dev_t setup(void)
{
    blk_dev_t dev;

    memset(disk, 0, sizeof(disk_words));
    disk[510] = 0x55;
    disk[511] = 0xAA;
    CHECK_EQ(blk_ram_init(&dev, disk, sizeof(disk_words), 0), 0);
    return dev;
}

TEST(ram_device)
{
    blk_dev_t dev = setup();
    unsigned char buf[BLK_SECTOR_SIZE * 2];

    CHECK_EQ(dev.block_count, BLOCKS);
    CHECK_EQ(dev.block_size, BLK_SECTOR_SIZE);
    memset(buf, 0xA5, sizeof(buf));
    CHECK_EQ(blk_write(&dev, BLOCKS - 2, 2, buf), 0);
    CHECK_EQ(disk[(BLOCKS - 1) * BLK_SECTOR_SIZE], 0xA5);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(blk_read(&dev, BLOCKS - 2, 2, buf), 0);
    CHECK_EQ(buf[BLK_SECTOR_SIZE + 7], 0xA5);
    CHECK_EQ(blk_read(&dev, BLOCKS - 1, 2, buf), BLK_ERR_RANGE);
    CHECK_EQ(blk_read(&dev, BLOCKS, 1, buf), BLK_ERR_RANGE);
    CHECK_EQ(blk_ram_init(&dev, disk, 511, 0), BLK_ERR_PARAM);
    CHECK_EQ(blk_ram_init(NULL, disk, 4096, 0), BLK_ERR_PARAM);
    CHECK_EQ(blk_ram_init(&dev, rc_launder(NULL), 4096, 0), BLK_ERR_PARAM);
    CHECK_EQ(blk_ram_init(&dev, disk, 1000, BLK_FLAG_READONLY), 0);
    CHECK_EQ(dev.block_count, 1);
    CHECK_EQ(blk_write(&dev, 0, 1, buf), BLK_ERR_RO);
}

TEST(mbr_primaries)
{
    blk_dev_t dev = setup();
    part_info_t p[4];

    entry(0, 0x80, PART_TYPE_FAT32_LBA, 2048, 1024);
    entry(2, 0x00, PART_TYPE_FAT16, 3072, 1024);
    CHECK_EQ(part_scan(&dev, p, 4), 2);
    CHECK_EQ(p[0].index, 0);
    CHECK_EQ(p[0].type, PART_TYPE_FAT32_LBA);
    CHECK_EQ(p[0].bootable, 1);
    CHECK_EQ(p[0].start_lba, 2048);
    CHECK_EQ(p[0].block_count, 1024);
    CHECK_EQ(p[0].superfloppy, 0);
    CHECK(p[0].dev == &dev);
    CHECK_EQ(p[1].index, 2);
    CHECK_EQ(p[1].bootable, 0);
    CHECK_EQ(p[1].start_lba, 3072);
    CHECK_EQ(part_scan(&dev, p, 1), 1);
    CHECK_EQ(part_scan(&dev, NULL, 0), 0);
}

TEST(mbr_skips_bad_entries)
{
    blk_dev_t dev = setup();
    part_info_t p[4];

    entry(0, 0, PART_TYPE_FAT16, 4000, 200); /* past the end */
    entry(1, 0, PART_TYPE_FAT16, 0, 10);     /* overlaps the MBR */
    entry(2, 0, 0, 100, 10);                 /* empty type */
    entry(3, 0, PART_TYPE_EXTENDED, 100, 10);
    CHECK_EQ(part_scan(&dev, p, 4), 1);
    CHECK_EQ(p[0].type, PART_TYPE_EXTENDED);
}

TEST(not_a_table)
{
    blk_dev_t dev = setup();
    part_info_t p[4];

    entry(0, 0x12, PART_TYPE_FAT16, 2048, 10); /* bad status byte */
    CHECK_EQ(part_scan(&dev, p, 4), 0);
    disk[510] = 0;
    entry(0, 0, PART_TYPE_FAT16, 2048, 10);
    CHECK_EQ(part_scan(&dev, p, 4), 0);
}

TEST(superfloppy)
{
    blk_dev_t dev = setup();
    part_info_t p[4];

    CHECK_EQ(fs_mkfs(&dev, NULL), FS_OK);
    CHECK_EQ(part_scan(&dev, p, 4), 1);
    CHECK_EQ(p[0].superfloppy, 1);
    CHECK_EQ(p[0].start_lba, 0);
    CHECK_EQ(p[0].block_count, BLOCKS);
    CHECK_EQ(part_scan(&dev, p, 0), 0);
}

TEST(errors)
{
    blk_dev_t dev = setup();
    blk_fault_t f;
    part_info_t p[4];

    CHECK_EQ(part_scan(rc_launder(NULL), p, 4), BLK_ERR_PARAM);
    CHECK_EQ(part_scan(&dev, rc_launder(NULL), 4), BLK_ERR_PARAM);
    blk_fault_init(&f, &dev);
    f.fail_read_after = 0;
    f.err = BLK_ERR_TIMEOUT;
    CHECK_EQ(part_scan(&f.dev, p, 4), BLK_ERR_TIMEOUT);
}

TEST(open_partition)
{
    blk_dev_t dev = setup();
    blk_dev_t sub;
    part_info_t p[4], bad;
    unsigned char buf[BLK_SECTOR_SIZE];

    entry(1, 0, PART_TYPE_FAT16, 1000, 100);
    CHECK_EQ(part_scan(&dev, p, 4), 1);
    CHECK_EQ(part_open(&dev, &p[0], &sub), 0);
    CHECK_EQ(sub.block_count, 100);
    memset(buf, 0x5A, sizeof(buf));
    CHECK_EQ(blk_write(&sub, 0, 1, buf), 0);
    CHECK_EQ(disk[1000 * BLK_SECTOR_SIZE], 0x5A);
    CHECK_EQ(disk[999 * BLK_SECTOR_SIZE], 0);
    CHECK_EQ(blk_write(&sub, 99, 1, buf), 0);
    CHECK_EQ(disk[1099 * BLK_SECTOR_SIZE], 0x5A);
    CHECK_EQ(disk[1100 * BLK_SECTOR_SIZE], 0);
    CHECK_EQ(blk_write(&sub, 100, 1, buf), BLK_ERR_RANGE);
    CHECK_EQ(blk_read(&sub, 99, 2, buf), BLK_ERR_RANGE);
    CHECK_EQ(blk_read(&sub, 99, 1, buf), 0);
    CHECK_EQ(blk_sync(&sub), 0);

    bad = p[0];
    bad.block_count = BLOCKS; /* runs past the device */
    CHECK_EQ(part_open(&dev, &bad, &sub), BLK_ERR_RANGE);
    bad = p[0];
    bad.dev = NULL;
    CHECK_EQ(part_open(&dev, &bad, &sub), BLK_ERR_PARAM);
    CHECK_EQ(part_open(&dev, rc_launder(NULL), &sub), BLK_ERR_PARAM);

    dev.flags = BLK_FLAG_READONLY;
    CHECK_EQ(part_open(&dev, &p[0], &sub), 0);
    CHECK_EQ(blk_write(&sub, 0, 1, buf), BLK_ERR_RO);
}

TEST(fs_in_partition)
{
    blk_dev_t dev = setup();
    blk_dev_t sub;
    part_info_t p[4];
    char buf[16];
    size_t n;

    entry(0, 0x80, PART_TYPE_FAT16_LBA, 64, BLOCKS - 64);
    CHECK_EQ(part_scan(&dev, p, 4), 1);
    CHECK_EQ(part_open(&dev, &p[0], &sub), 0);
    CHECK_EQ(fs_mkfs(&sub, NULL), FS_OK);
    CHECK_EQ(fs_mount("p0", &sub), FS_OK);
    CHECK_EQ(fs_write_file_atomic("p0:/HELLO.TXT", "partition", 9), FS_OK);
    CHECK_EQ(fs_unmount("p0"), FS_OK);
    /* The MBR is untouched and the partition still mounts. */
    CHECK_EQ(part_scan(&dev, p, 4), 1);
    CHECK_EQ(p[0].start_lba, 64);
    CHECK_EQ(part_open(&dev, &p[0], &sub), 0);
    CHECK_EQ(fs_mount("p0", &sub), FS_OK);
    CHECK_EQ(fs_read_file("p0:/HELLO.TXT", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 9);
    CHECK_MEM(buf, "partition", 9);
    CHECK_EQ(fs_unmount("p0"), FS_OK);
}

/* Partitions of one disk: overlapping ranges cannot be mounted twice. */
TEST(mount_overlapping_partitions)
{
    blk_dev_t dev = setup();
    part_info_t p[4], wide;
    blk_dev_t v0, v0_again, v1, v_wide;
    uint64_t start = 99;

    entry(0, 0x00, PART_TYPE_FAT16, 64, 1024);
    entry(1, 0x00, PART_TYPE_FAT16, 2048, 1024);
    CHECK_EQ(part_scan(&dev, p, 4), 2);
    CHECK_EQ(part_open(&dev, &p[0], &v0), 0);
    CHECK_EQ(part_open(&dev, &p[0], &v0_again), 0);
    CHECK_EQ(part_open(&dev, &p[1], &v1), 0);
    CHECK(part_parent_of(&v1, &start) == &dev);
    CHECK_EQ(start, 2048);
    CHECK(part_parent_of(&dev, &start) == NULL);
    CHECK(part_parent_of(rc_launder(NULL), NULL) == NULL);
    CHECK_EQ(fs_mkfs(&v0, NULL), FS_OK);
    CHECK_EQ(fs_mkfs(&v1, NULL), FS_OK);

    CHECK_EQ(fs_mount("p0", &v0), FS_OK);
    CHECK_EQ(fs_mount("again", &v0_again), FS_ERR_MOUNTED);
    CHECK_EQ(fs_mount("whole", &dev), FS_ERR_MOUNTED);
    CHECK_EQ(fs_mkfs(&dev, NULL), FS_ERR_MOUNTED);
    CHECK_EQ(fs_mkfs(&v0_again, NULL), FS_ERR_MOUNTED);
    /* A range that starts inside partition 0. */
    wide = p[1];
    wide.start_lba = 1000;
    wide.block_count = 1100;
    CHECK_EQ(part_open(&dev, &wide, &v_wide), 0);
    CHECK_EQ(fs_mount("wide", &v_wide), FS_ERR_MOUNTED);
    /* The disjoint partition mounts. */
    CHECK_EQ(fs_mount("p1", &v1), FS_OK);
    CHECK_EQ(fs_write_file_atomic("p1:/B.TXT", "b", 1), FS_OK);
    CHECK_EQ(fs_unmount("p0"), FS_OK);
    CHECK_EQ(fs_mount("again", &v0_again), FS_OK);
    CHECK_EQ(fs_unmount("again"), FS_OK);
    CHECK_EQ(fs_unmount("p1"), FS_OK);
}
