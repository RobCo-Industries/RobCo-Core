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
    CHECK_EQ(fs_stat("v:/~SAVE.DAT", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
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
    CHECK_EQ(fs_stat("v:/PIPBOY/~SAVE1.SAV", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
    /* A stale temp file from an earlier crash is replaced. */
    CHECK_EQ(fs_write_file_atomic("v:/PIPBOY/SAVE1.SAV", "x", 1), FS_OK);
    CHECK_EQ(fs_rename("v:/PIPBOY/SAVE1.SAV", "v:/PIPBOY/~SAVE1.SAV"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/PIPBOY/SAVE1.SAV", "", 0), FS_OK);
    CHECK_EQ(fs_read_file("v:/PIPBOY/SAVE1.SAV", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 0);
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

/*
 * Crash tests for fs_write_file_atomic(). Each run formats the disk,
 * optionally writes the old content, then replaces it through a fault
 * device with one fault at index k: a failed write call, a failed sync,
 * or a power cut after block k (later blocks are dropped and the calls
 * still succeed). k grows until the call runs with no fault hit. After
 * each run the test "reboots": it forgets the FatFs state without a
 * flush, mounts the RAM disk again, recovers, and checks the target.
 */
enum { FAULT_WRITE, FAULT_SYNC, FAULT_DROP };

#define CRASH_DIR    "v:/S"
#define CRASH_TARGET "v:/S/GAME.SAV"
#define CRASH_MAX    4000

typedef struct {
    int mode;
    int had_old;  /* replace an existing file, or a first write */
    int crowded;  /* entries spread over two directory sectors */
    long points;  /* fault points run */
    int saw_old, saw_new, saw_absent, saw_leftover;
} crash_t;

static unsigned char crash_old[3000], crash_new[5000], crash_buf[8000];

static int count_entries(const char *dir, int *leftovers);

static void crash_setup(const crash_t *c)
{
    char name[24];
    fs_file_t f;
    int i;

    format(DISK_SIZE);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    CHECK_EQ(fs_mkdir(CRASH_DIR), FS_OK);
    if (c->crowded) {
        /*
         * 20 files, then the target in the second 16-entry sector, then
         * two free slots in the first: new entries from the renames land
         * in another sector than the entry they replace.
         */
        for (i = 0; i < 20; i++) {
            snprintf(name, sizeof(name), CRASH_DIR "/F%02d", i);
            CHECK_EQ(fs_open(&f, name, FS_WRITE | FS_CREATE), FS_OK);
            CHECK_EQ(fs_close(&f), FS_OK);
        }
    }
    if (c->had_old) {
        CHECK_EQ(fs_write_file_atomic(CRASH_TARGET, crash_old, sizeof(crash_old)), FS_OK);
    }
    if (c->crowded) {
        CHECK_EQ(fs_unlink(CRASH_DIR "/F00"), FS_OK);
        CHECK_EQ(fs_unlink(CRASH_DIR "/F01"), FS_OK);
    }
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

/* After recovery: exactly old, exactly new, or absent for a first write. */
static void crash_check(crash_t *c, int finished, long k)
{
    static unsigned char fill_data[16384];
    unsigned char *want = NULL;
    size_t n = 0, want_len = 0;
    fs_file_t f;
    int got, left;

    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    count_entries(CRASH_DIR, &left);
    c->saw_leftover |= left > 0;
    if (k % 2 == 0) {
        got = fs_read_file_atomic(CRASH_TARGET, crash_buf, sizeof(crash_buf), &n);
    } else {
        CHECK_EQ(fs_atomic_recover(CRASH_TARGET), FS_OK);
        got = fs_read_file(CRASH_TARGET, crash_buf, sizeof(crash_buf), &n);
    }
    if (got == FS_ERR_NOENT) {
        CHECK(!c->had_old);
        CHECK(!finished);
        c->saw_absent = 1;
    } else {
        CHECK_EQ(got, FS_OK);
        if (n == sizeof(crash_new) && memcmp(crash_buf, crash_new, n) == 0) {
            want = crash_new;
            c->saw_new = 1;
        } else {
            CHECK(c->had_old && !finished);
            CHECK_EQ(n, sizeof(crash_old));
            CHECK_MEM(crash_buf, crash_old, sizeof(crash_old));
            want = crash_old;
            c->saw_old = 1;
        }
        want_len = n;
    }
    /* Recovery leaves no work file behind. */
    count_entries(CRASH_DIR, &left);
    CHECK_EQ(left, 0);
    /* New allocations must not reuse the target's clusters. */
    fill(fill_data, sizeof(fill_data), 7);
    CHECK_EQ(fs_open(&f, CRASH_DIR "/FILL.BIN", FS_WRITE | FS_CREATE | FS_TRUNC), FS_OK);
    CHECK_EQ(fs_write(&f, fill_data, sizeof(fill_data)), (int)sizeof(fill_data));
    CHECK_EQ(fs_close(&f), FS_OK);
    if (want != NULL) {
        CHECK_EQ(fs_read_file(CRASH_TARGET, crash_buf, sizeof(crash_buf), &n), FS_OK);
        CHECK_EQ(n, want_len);
        CHECK_MEM(crash_buf, want, want_len);
    }
    /* The next save works. */
    CHECK_EQ(fs_write_file_atomic(CRASH_TARGET, crash_old, 10), FS_OK);
    CHECK_EQ(fs_unmount("v"), FS_OK);
}

static void crash_run(crash_t *c)
{
    blk_fault_t fault;
    long k;
    int rc, finished = 0;

    fill(crash_old, sizeof(crash_old), 3);
    fill(crash_new, sizeof(crash_new), 4);
    for (k = 0; k < CRASH_MAX && !finished; k++) {
        crash_setup(c);
        blk_fault_init(&fault, &ram);
        if (c->mode == FAULT_WRITE) {
            fault.fail_write_after = k;
        } else if (c->mode == FAULT_SYNC) {
            fault.fail_sync_after = k;
        } else {
            fault.drop_write_after = k;
        }
        CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
        rc = fs_write_file_atomic(CRASH_TARGET, crash_new, sizeof(crash_new));
        if (c->mode == FAULT_WRITE) {
            finished = fault.writes <= (unsigned long)k;
        } else if (c->mode == FAULT_SYNC) {
            finished = fault.syncs <= (unsigned long)k;
        } else {
            finished = fault.dropped == 0;
        }
        /*
         * A failed write may still return FS_OK: FatFs ignores errors on
         * the second FAT copy. Only the outcome after recovery counts.
         */
        if (finished) {
            CHECK_EQ(rc, FS_OK);
        }
        /* Reboot: drop the FatFs state without writing it back. */
        fault.fail_sync_after = -1;
        CHECK_EQ(fs_unmount("v"), FS_OK);
        crash_check(c, finished, k);
    }
    CHECK(finished);
    c->points = k - 1; /* the last run hit no fault */
}

static void crash_report(const char *what, const crash_t *c)
{
    printf("     %s: %ld fault points\n", what, c->points);
}

TEST(atomic_replace_crash_every_write)
{
    crash_t c = { FAULT_WRITE, 1, 0, 0, 0, 0, 0, 0 };

    crash_run(&c);
    crash_report("replace, failed write", &c);
    CHECK(c.points > 5);
    CHECK(c.saw_old && c.saw_new && c.saw_leftover);
}

TEST(atomic_replace_crash_every_sync)
{
    crash_t c = { FAULT_SYNC, 1, 0, 0, 0, 0, 0, 0 };

    crash_run(&c);
    crash_report("replace, failed sync", &c);
    CHECK(c.points >= 4);
    CHECK(c.saw_old && c.saw_new);
}

TEST(atomic_replace_power_cut)
{
    crash_t c = { FAULT_DROP, 1, 0, 0, 0, 0, 0, 0 };

    crash_run(&c);
    crash_report("replace, power cut", &c);
    CHECK(c.saw_old && c.saw_new && c.saw_leftover);
}

/* Renames that move an entry to another sector pass through an alias. */
TEST(atomic_replace_crowded_directory)
{
    crash_t w = { FAULT_WRITE, 1, 1, 0, 0, 0, 0, 0 };
    crash_t d = { FAULT_DROP, 1, 1, 0, 0, 0, 0, 0 };

    crash_run(&w);
    crash_report("crowded, failed write", &w);
    crash_run(&d);
    crash_report("crowded, power cut", &d);
    CHECK(w.saw_old && w.saw_new && d.saw_old && d.saw_new);
}

TEST(atomic_first_write_crash)
{
    crash_t w = { FAULT_WRITE, 0, 0, 0, 0, 0, 0, 0 };
    crash_t s = { FAULT_SYNC, 0, 0, 0, 0, 0, 0, 0 };
    crash_t d = { FAULT_DROP, 0, 0, 0, 0, 0, 0, 0 };

    crash_run(&w);
    crash_report("first write, failed write", &w);
    crash_run(&s);
    crash_report("first write, failed sync", &s);
    crash_run(&d);
    crash_report("first write, power cut", &d);
    CHECK(w.saw_absent && w.saw_new && d.saw_absent && d.saw_new);
}

static void put_file(const char *path, const char *text)
{
    fs_file_t f;

    CHECK_EQ(fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC), FS_OK);
    CHECK_EQ(fs_write(&f, text, strlen(text)), (int)strlen(text));
    CHECK_EQ(fs_close(&f), FS_OK);
}

/* Each leftover state, built by hand. */
TEST(atomic_recovery_table)
{
    static const struct {
        const char *t, *n, *o; /* content of target, ~ and #, NULL if absent */
        const char *want;      /* target after recovery, NULL if absent */
    } cases[] = {
        { "T", NULL, NULL, "T" },
        { "T", NULL, "O", "T" },  /* replaced, old copy not yet removed */
        { "T", "N", NULL, "T" },  /* new copy not complete: dropped */
        { "T", "N", "O", "T" },
        { NULL, "N", "O", "N" },  /* roll forward */
        { NULL, NULL, "O", "O" }, /* roll back */
        { NULL, "N", NULL, NULL },/* first write cut short: dropped */
        { NULL, NULL, NULL, NULL },
    };
    char buf[8];
    size_t i, n;
    int left;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        format(DISK_SIZE);
        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        if (cases[i].t) put_file("v:/F.SAV", cases[i].t);
        if (cases[i].n) put_file("v:/~F.SAV", cases[i].n);
        if (cases[i].o) put_file("v:/#F.SAV", cases[i].o);
        put_file("v:/OTHER.SAV", "x");
        CHECK_EQ(fs_unmount("v"), FS_OK);

        /* A read-only volume reads the right copy and changes nothing. */
        ram.flags |= BLK_FLAG_READONLY;
        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        n = 99;
        if (cases[i].want != NULL) {
            CHECK_EQ(fs_read_file_atomic("v:/F.SAV", buf, sizeof(buf), &n), FS_OK);
            CHECK_EQ(n, 1);
            CHECK_EQ(buf[0], cases[i].want[0]);
        } else {
            CHECK_EQ(fs_read_file_atomic("v:/F.SAV", buf, sizeof(buf), &n), FS_ERR_NOENT);
            CHECK_EQ(n, 0);
        }
        CHECK_EQ(fs_atomic_recover("v:/F.SAV"),
                 cases[i].n || cases[i].o ? FS_ERR_RO : FS_OK);
        CHECK_EQ(fs_unmount("v"), FS_OK);
        ram.flags &= ~BLK_FLAG_READONLY;

        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        CHECK_EQ(fs_atomic_recover("v:/F.SAV"), FS_OK);
        if (cases[i].want != NULL) {
            CHECK_EQ(fs_read_file("v:/F.SAV", buf, sizeof(buf), &n), FS_OK);
            CHECK_EQ(buf[0], cases[i].want[0]);
        } else {
            CHECK_EQ(fs_stat("v:/F.SAV", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
        }
        CHECK_EQ(count_entries("v:/", &left), cases[i].want ? 2 : 1);
        CHECK_EQ(left, 0);
        CHECK_EQ(fs_atomic_recover("v:/F.SAV"), FS_OK);
        CHECK_EQ(fs_unmount("v"), FS_OK);
    }
    CHECK_EQ(fs_atomic_recover("v:/~F.SAV"), FS_ERR_NAME);
    CHECK_EQ(fs_read_file_atomic(rc_launder(NULL), buf, sizeof(buf), &n), FS_ERR_INVAL);
    CHECK_EQ(fs_read_file_atomic("v:/F.SAV", rc_launder(NULL), 1, &n), FS_ERR_INVAL);
}

static int count_entries(const char *dir, int *leftovers)
{
    fs_dir_t d;
    fs_stat_t st;
    int n = 0, rc;

    *leftovers = 0;
    CHECK_EQ(fs_opendir(&d, dir), FS_OK);
    while ((rc = fs_readdir(&d, &st)) == 1) {
        n++;
        *leftovers += st.leftover;
    }
    CHECK_EQ(rc, 0);
    CHECK_EQ(fs_closedir(&d), FS_OK);
    return n;
}

/* Work files use reserved names that no ordinary file shares. */
TEST(atomic_work_names)
{
    char buf[64];
    size_t n;
    int left;
    fs_file_t f;
    fs_stat_t st;

    format(DISK_SIZE);
    CHECK_EQ(fs_mount("v", &ram), FS_OK);
    /* An unrelated file with the old temp name survives. */
    CHECK_EQ(fs_write_file_atomic("v:/SAVE1.TMP", "mine", 4), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/SAVE1.SAV", "save", 4), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/SAVE1.SAV", "save2", 5), FS_OK);
    CHECK_EQ(fs_read_file("v:/SAVE1.TMP", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 4);
    CHECK_MEM(buf, "mine", 4);
    /* Same base, other extension: separate work files. */
    CHECK_EQ(fs_write_file_atomic("v:/A.SAV", "sav", 3), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/A.DAT", "dat", 3), FS_OK);
    CHECK_EQ(fs_open(&f, "v:/~A.SAV", FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_write(&f, "partial", 7), 7);
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/A.DAT", "dat2", 4), FS_OK);
    CHECK_EQ(fs_read_file("v:/A.SAV", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 3);
    CHECK_MEM(buf, "sav", 3);
    CHECK_EQ(fs_read_file("v:/A.DAT", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, 4);
    CHECK_MEM(buf, "dat2", 4);
    /* Listings flag work files; 8.3 targets keep 8.3 work names. */
    CHECK_EQ(count_entries("v:/", &left), 5);
    CHECK_EQ(left, 1);
    CHECK_EQ(fs_stat("v:/~A.SAV", &st), FS_OK);
    CHECK_EQ(st.leftover, 1);
    CHECK_STR(st.name, "~A.SAV");
    CHECK_STR(st.short_name, ""); /* a plain 8.3 entry, no long name */
    CHECK_EQ(fs_stat("v:/A.SAV", &st), FS_OK);
    CHECK_EQ(st.leftover, 0);
    /* Reserved names are refused as targets. */
    CHECK_EQ(fs_write_file_atomic("v:/~A.SAV", "x", 1), FS_ERR_NAME);
    CHECK_EQ(fs_write_file_atomic("v:/#A.SAV", "x", 1), FS_ERR_NAME);
    /* An 8-character base gets long work names and still works. */
    CHECK_EQ(fs_write_file_atomic("v:/SAVEGAME.SAV", "g1", 2), FS_OK);
    CHECK_EQ(fs_write_file_atomic("v:/SAVEGAME.SAV", "g2", 2), FS_OK);
    CHECK_EQ(fs_read_file("v:/SAVEGAME.SAV", buf, sizeof(buf), &n), FS_OK);
    CHECK_MEM(buf, "g2", 2);
    CHECK_EQ(fs_unmount("v"), FS_OK);
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
    /* fs: the code at mount time. live: on a mounted volume, where no medium means gone. */
    static const struct {
        int blk;
        int fs;
        int live;
    } map[] = {
        { BLK_ERR_IO, FS_ERR_IO, FS_ERR_IO },
        { BLK_ERR_TIMEOUT, FS_ERR_TIMEOUT, FS_ERR_TIMEOUT },
        { BLK_ERR_NOMEDIA, FS_ERR_NOTREADY, FS_ERR_GONE },
        { BLK_ERR_RANGE, FS_ERR_CORRUPT, FS_ERR_CORRUPT },
        { BLK_ERR_PARAM, FS_ERR_INVAL, FS_ERR_INVAL },
        { BLK_ERR_RO, FS_ERR_RO, FS_ERR_RO },
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
        CHECK_EQ(fs_mkdir("v:/X"), map[i].live);
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

/*
 * A write that fails, then a close once the device works again: the file
 * keeps the bytes fs_write() reported, plus at most the part of the failed
 * call that FatFs had already taken in. Every byte is the caller's.
 */
TEST(close_after_failed_write)
{
    static unsigned char data[6000], back[8000];
    blk_fault_t fault;
    fs_file_t f;
    long k;
    int rc, failed = 0, longer = 0;
    size_t n, done;

    fill(data, sizeof(data), 8);
    for (k = 0; k < 40; k++) {
        format(DISK_SIZE);
        blk_fault_init(&fault, &ram);
        CHECK_EQ(fs_mount("v", &fault.dev), FS_OK);
        CHECK_EQ(fs_open(&f, "v:/LOG.BIN", FS_WRITE | FS_CREATE), FS_OK);
        fault.fail_write_after = (long)fault.writes + k;
        done = 0;
        while (done < sizeof(data)) {
            size_t want = sizeof(data) - done < 700 ? sizeof(data) - done : 700;

            rc = fs_write(&f, data + done, want);
            if (rc < 0) {
                failed++;
                break;
            }
            done += (size_t)rc;
        }
        fault.fail_write_after = -1;
        fs_close(&f);
        CHECK_EQ(fs_unmount("v"), FS_OK);
        CHECK_EQ(fs_mount("v", &ram), FS_OK);
        CHECK_EQ(fs_read_file("v:/LOG.BIN", back, sizeof(back), &n), FS_OK);
        CHECK(n >= done && n <= done + 700);
        CHECK_MEM(back, data, n);
        longer += n > done;
        CHECK_EQ(fs_unmount("v"), FS_OK);
    }
    CHECK(failed > 0);
    CHECK(longer > 0);
}
