/* File API under stress: full volume, atomic writes, read-only and failing devices. */
#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/fs.h>

#include "blk_file.h"
#include "rc_test.h"

#define DISK_SIZE (1024u * 1024u)
#define SMALL_SIZE (160u * 1024u)
#define MAX_STEPS 400

static uint64_t disk_words[DISK_SIZE / 8];
static unsigned char *const disk = (unsigned char *)disk_words;
static blk_dev_t ram;

static void format(size_t size)
{
    fs_unmount("v");
    memset(disk, 0, DISK_SIZE);
    CHECK_EQ(blk_ram_init(&ram, disk, size, 0), 0);
    CHECK_EQ(fs_mkfs(&ram, NULL), FS_OK);
}

static void fill(unsigned char *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(i * 31 + seed);
    }
}

TEST(full_volume)
{
    static unsigned char keep[8000], chunk[4096], back[SMALL_SIZE];
    fs_statvfs_t st;
    fs_file_t f;
    size_t n, total = 0;
    int rc = 0, steps = 0;

    format(SMALL_SIZE);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    fill(keep, sizeof(keep), 1);
    CHECK_EQ(fs_write_file_atomic("v:/KEEP.BIN", keep, sizeof(keep)), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/SAVE.DAT", "old save", 8), FS_OK);
    fill(chunk, sizeof(chunk), 2);
    CHECK_EQ(fs_open(&f, "v:/BIG.BIN", FS_WRITE | FS_CREATE), FS_OK);
    while (steps++ < MAX_STEPS) {
        rc = fs_write(&f, chunk, sizeof(chunk));
        if (rc < 0) {
            break;
        }
        total += (size_t)rc;
    }
    CHECK_EQ(rc, FS_ERR_NOSPC);
    CHECK(steps < MAX_STEPS);
    CHECK_EQ(fs_size(&f), total);
    CHECK_EQ(fs_write(&f, chunk, 1), FS_ERR_NOSPC);
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_statvfs("v", &st), FS_OK);
    CHECK(st.free_bytes < st.cluster_bytes);

    /* Creating and growing fails cleanly; earlier files stay intact. */
    CHECK_EQ(fs_write_file_atomic("v:/SAVE.DAT", keep, sizeof(keep)), FS_ERR_NOSPC);
    CHECK_EQ(fs_stat("v:/SAVE.TMP", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
    CHECK_EQ(fs_read_file("v:/SAVE.DAT", back, sizeof(back), &n), FS_OK);
    CHECK_EQ(n, 8);
    CHECK_MEM(back, "old save", 8);
    CHECK_EQ(fs_unmount("v"), FS_OK);

    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    CHECK_EQ(fs_read_file("v:/KEEP.BIN", back, sizeof(back), &n), FS_OK);
    CHECK_EQ(n, sizeof(keep));
    CHECK_MEM(back, keep, sizeof(keep));
    CHECK_EQ(fs_read_file("v:/BIG.BIN", back, sizeof(back), &n), FS_OK);
    CHECK_EQ(n, total);
    for (n = 0; n + sizeof(chunk) <= total; n += sizeof(chunk)) {
        CHECK_MEM(back + n, chunk, sizeof(chunk));
    }
    /* Space comes back. */
    CHECK_EQ(fs_unlink("v:/BIG.BIN"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/SAVE.DAT", keep, sizeof(keep)), FS_OK);
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

TEST(full_root_directory)
{
    char name[16];
    fs_file_t f;
    int i, rc = 0;

    format(SMALL_SIZE);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    for (i = 0; i < 2000; i++) {
        snprintf(name, sizeof(name), "v:/F%d", i);
        rc = fs_open(&f, name, FS_WRITE | FS_CREATE);
        if (rc != FS_OK) {
            break;
        }
        CHECK_EQ(fs_close(&f), FS_OK);
    }
    CHECK(i < 2000);
    CHECK_EQ(rc, FS_ERR_DENIED); /* FAT12 root holds a fixed number of entries */
    CHECK_EQ(fs_stat("v:/F0", &(fs_stat_t){ 0 }), FS_OK);
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

TEST(atomic_write_success)
{
    char buf[64];
    size_t n;

    format(DISK_SIZE);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    CHECK_EQ(fs_mkdir("v:/PIPBOY"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/PIPBOY/SAVE1.SAV", "first", 5), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/PIPBOY/SAVE1.SAV", "second!", 7), FS_OK);
    CHECK_EQ(fs_read_file("v:/PIPBOY/SAVE1.SAV", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 7);
    CHECK_MEM(buf, "second!", 7);
    CHECK_EQ(fs_stat("v:/PIPBOY/SAVE1.TMP", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
    /* A stale temp file from an earlier crash is replaced. */
    CHECK_EQ(fs_write_file_atomic("v:/PIPBOY/SAVE1.SAV", "x", 1), FS_OK);
    CHECK_EQ(fs_rename("v:/PIPBOY/SAVE1.SAV", "v:/PIPBOY/SAVE1.TMP"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/PIPBOY/SAVE1.SAV", "", 0), FS_OK);
    CHECK_EQ(fs_read_file("v:/PIPBOY/SAVE1.SAV", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 0);
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

/*
 * Fails every device write after the first k, for growing k, during an
 * atomic replace. After each failure a fresh mount must find the old
 * content, the new content, or (only between unlink and rename) the new
 * content in the temp file.
 */
TEST(atomic_write_survives_failures)
{
    static unsigned char old_data[3000], new_data[5000], buf[8000];
    blk_fault_t fault;
    long k;
    int rc = -1, saw_old = 0, saw_window = 0;
    size_t n;

    fill(old_data, sizeof(old_data), 3);
    fill(new_data, sizeof(new_data), 4);
    for (k = 0; k < MAX_STEPS && rc != FS_OK; k++) {
        format(DISK_SIZE);
        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        CHECK_EQ(fs_mkdir("v:/SAVES"), FS_OK);
        CHECK_EQ(fs_write_file_atomic("v:/SAVES/GAME.SAV", old_data, sizeof(old_data)), FS_OK);
        CHECK_EQ(fs_unmount("v"), FS_OK);

        blk_fault_init(&fault, &ram);
        fault.fail_write_after = k;
        CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
        rc = fs_write_file_atomic("v:/SAVES/GAME.SAV", new_data, sizeof(new_data));
        CHECK(rc == FS_OK || rc == FS_ERR_IO);
        CHECK_EQ(fs_unmount("v"), FS_OK);

        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        int got = fs_read_file("v:/SAVES/GAME.SAV", buf, sizeof(buf), &n);
        if (rc == FS_OK) {
            CHECK_EQ(got, FS_OK);
            CHECK_EQ(n, sizeof(new_data));
            CHECK_MEM(buf, new_data, sizeof(new_data));
        } else if (got == FS_OK && n == sizeof(old_data)) {
            CHECK_MEM(buf, old_data, sizeof(old_data));
            saw_old = 1;
        } else if (got == FS_OK) {
            CHECK_EQ(n, sizeof(new_data));
            CHECK_MEM(buf, new_data, sizeof(new_data));
        } else {
            CHECK_EQ(got, FS_ERR_NOENT);
            CHECK_EQ(fs_read_file("v:/SAVES/GAME.TMP", buf, sizeof(buf), &n), FS_OK);
            CHECK_EQ(n, sizeof(new_data));
            CHECK_MEM(buf, new_data, sizeof(new_data));
            saw_window = 1;
        }
        CHECK_EQ(fs_unmount("v"), FS_OK);
    }
    CHECK_EQ(rc, FS_OK);
    CHECK(k > 3);
    CHECK(saw_old);
    CHECK(saw_window);
}

TEST(readonly_device)
{
    fs_statvfs_t st;
    fs_file_t f;
    char buf[16];
    size_t n;

    format(DISK_SIZE);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    CHECK_EQ(fs_mkdir("v:/D"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/D/F.TXT", "ro", 2), FS_OK);
    CHECK_EQ(fs_unmount("v"), FS_OK);

    ram.flags |= BLK_FLAG_READONLY;
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    CHECK_EQ(fs_read_file("v:/D/F.TXT", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 2);
    CHECK_EQ(fs_statvfs("v", &st), FS_OK);
    CHECK_EQ(fs_open(&f, "v:/D/F.TXT", FS_WRITE), FS_ERR_RO);
    CHECK_EQ(fs_open(&f, "v:/D/NEW.TXT", FS_WRITE | FS_CREATE), FS_ERR_RO);
    CHECK_EQ(fs_mkdir("v:/E"), FS_ERR_RO);
    CHECK_EQ(fs_unlink("v:/D/F.TXT"), FS_ERR_RO);
    CHECK_EQ(fs_rename("v:/D/F.TXT", "v:/D/G.TXT"), FS_ERR_RO);
    CHECK_EQ(fs_write_file_atomic("v:/D/F.TXT", "x", 1), FS_ERR_RO);
    CHECK_EQ(fs_unmount("v"), FS_OK);
    CHECK_EQ(fs_mkfs(&ram, NULL), FS_ERR_RO);
    ram.flags &= ~BLK_FLAG_READONLY;
}

/* Fails reads after the first k: every call returns, none loops. */
TEST(read_errors)
{
    static unsigned char data[20000], buf[20000];
    blk_fault_t fault;
    long k;
    int rc = -1, mounted;
    size_t n;

    format(DISK_SIZE);
    fill(data, sizeof(data), 5);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    CHECK_EQ(fs_mkdir("v:/A"), FS_OK);
    CHECK_EQ(fs_mkdir("v:/A/B"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/A/B/DATA.BIN", data, sizeof(data)), FS_OK);
    CHECK_EQ(fs_unmount("v"), FS_OK);
    for (k = 0; k < MAX_STEPS && rc != FS_OK; k++) {
        blk_fault_init(&fault, &ram);
        fault.fail_read_after = k;
        fault.err = BLK_ERR_IO;
        mounted = fs_mount("v", &fault.dev);
        if (mounted != FS_OK) {
            CHECK_EQ(mounted, FS_ERR_IO);
            rc = mounted;
            continue;
        }
        rc = fs_read_file("v:/A/B/DATA.BIN", buf, sizeof(buf), &n);
        CHECK(rc == FS_OK || rc == FS_ERR_IO);
        if (rc == FS_OK) {
            CHECK_EQ(n, sizeof(data));
            CHECK_MEM(buf, data, sizeof(data));
        }
        CHECK_EQ(fs_unmount("v"), FS_OK);
    }
    CHECK_EQ(rc, FS_OK);
    CHECK(k > 3);
}

/* Fails writes after the first k during a mixed workload. */
TEST(write_errors)
{
    static unsigned char data[6000];
    blk_fault_t fault;
    fs_file_t f;
    long k;
    int rc = -1;

    fill(data, sizeof(data), 6);
    for (k = 0; k < MAX_STEPS && rc != FS_OK; k++) {
        format(DISK_SIZE);
        blk_fault_init(&fault, &ram);
        fault.fail_write_after = k;
        CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
        rc = fs_mkdir("v:/DIR");
        if (rc == FS_OK) {
            rc = fs_open(&f, "v:/DIR/LOG.TXT", FS_WRITE | FS_CREATE);
            if (rc == FS_OK) {
                int w = fs_write(&f, data, sizeof(data));
                int s = fs_sync(&f);
                int c = fs_close(&f);

                rc = w < 0 ? w : s != FS_OK ? s : c;
                CHECK(w < 0 || w == (int)sizeof(data));
            }
        }
        if (rc == FS_OK) {
            rc = fs_rename("v:/DIR/LOG.TXT", "v:/LOG.TXT");
        }
        CHECK(rc == FS_OK || rc == FS_ERR_IO);
        CHECK_EQ(fs_unmount("v"), FS_OK);
        /* Whatever happened, the volume still mounts. */
        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        CHECK_EQ(fs_unmount("v"), FS_OK);
    }
    CHECK_EQ(rc, FS_OK);
}

TEST(device_error_codes)
{
    static const struct {
        int blk;
        int fs;
    } map[] = {
        { BLK_ERR_IO, FS_ERR_IO },
        { BLK_ERR_TIMEOUT, FS_ERR_TIMEOUT },
        { BLK_ERR_NOMEDIA, FS_ERR_NOTREADY },
        { BLK_ERR_RANGE, FS_ERR_CORRUPT },
        { BLK_ERR_PARAM, FS_ERR_INVAL },
        { BLK_ERR_RO, FS_ERR_RO },
    };
    blk_fault_t fault;
    size_t i;

    format(DISK_SIZE);
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        blk_fault_init(&fault, &ram);
        fault.fail_read_after = 0;
        fault.err = map[i].blk;
        CHECK_EQ(fs_mount("v", &fault.dev), map[i].fs);

        blk_fault_init(&fault, &ram);
        fault.fail_write_after = 0;
        fault.err = map[i].blk;
        CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
        CHECK_EQ(fs_mkdir("v:/X"), map[i].fs);
        CHECK_EQ(fs_unmount("v"), FS_OK);
    }
}

/* A failed flush at unmount is reported and the volume stays mounted. */
TEST(sync_fails_at_unmount)
{
    blk_fault_t fault;
    fs_stat_t st;

    format(DISK_SIZE);
    blk_fault_init(&fault, &ram);
    CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/A.TXT", "abc", 3), FS_OK);
    fault.fail_sync_after = (long)fault.syncs;
    fault.err = BLK_ERR_TIMEOUT;
    CHECK_EQ(fs_unmount("v"), FS_ERR_TIMEOUT);
    CHECK_EQ(fs_stat("v:/A.TXT", &st), FS_OK);
    CHECK_EQ(fs_mount("v", &ram), FS_ERR_BUSY);
    fault.fail_sync_after = -1;
    CHECK_EQ(fs_unmount("v"), FS_OK);
    CHECK_EQ(fs_unmount("v"), FS_ERR_NOVOL);
}

TEST(sync_fails_at_close)
{
    blk_fault_t fault;
    fs_file_t f;

    format(DISK_SIZE);
    blk_fault_init(&fault, &ram);
    CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
    CHECK_EQ(fs_open(&f, "v:/B.TXT", FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_write(&f, "hello", 5), 5);
    fault.fail_sync_after = (long)fault.syncs;
    CHECK_EQ(fs_close(&f), FS_ERR_IO);
    CHECK_EQ(fs_close(&f), FS_ERR_BADF);
    fault.fail_sync_after = -1;
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

TEST(sync_fails_in_atomic_write)
{
    blk_fault_t fault;
    char buf[8];
    size_t n;

    format(DISK_SIZE);
    blk_fault_init(&fault, &ram);
    CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/C.TXT", "old", 3), FS_OK);
    fault.fail_sync_after = (long)fault.syncs;
    CHECK_EQ(fs_write_file_atomic("v:/C.TXT", "newer", 5), FS_ERR_IO);
    fault.fail_sync_after = -1;
    /* The old content is still there. */
    CHECK_EQ(fs_read_file("v:/C.TXT", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 3);
    CHECK_MEM(buf, "old", 3);
    CHECK_EQ(fs_unmount("v"), FS_OK);
}
