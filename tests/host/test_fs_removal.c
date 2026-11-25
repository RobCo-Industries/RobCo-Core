/* Two volumes, a removable medium pulled at every device call, read-only mounts. */
#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/fs.h>

#include "rc_test.h"

#define DISK_SIZE (512u * 1024u)
#define BIG_SIZE  6000u

/*
 * A card slot. The card is a RAM image. remove_at = N lets N device calls
 * through, then the card is out: every call returns BLK_ERR_NOMEDIA and
 * counts in calls_out. insert() puts a card back.
 */
typedef struct {
    blk_dev_t dev;
    blk_dev_t ram;
    unsigned calls, calls_out, writes;
    long remove_at;
    int present;
} slot_t;

static uint64_t boot_words[DISK_SIZE / 8], card_a[DISK_SIZE / 8], card_b[DISK_SIZE / 8];
static uint64_t tmpl_a[DISK_SIZE / 8], tmpl_b[DISK_SIZE / 8];
static blk_dev_t boot_dev;
static slot_t tape;

static int slot_gate(slot_t *s, int write)
{
    s->calls++;
    if (s->present && s->remove_at >= 0 && (long)s->calls > s->remove_at) {
        s->present = 0;
    }
    if (!s->present) {
        s->calls_out++;
        return BLK_ERR_NOMEDIA;
    }
    s->writes += (unsigned)write;
    return 0;
}

static int slot_read(blk_dev_t *d, uint64_t lba, uint32_t n, void *buf)
{
    slot_t *s = d->priv;
    int rc = slot_gate(s, 0);

    return rc != 0 ? rc : blk_read(&s->ram, lba, n, buf);
}

static int slot_write(blk_dev_t *d, uint64_t lba, uint32_t n, const void *buf)
{
    slot_t *s = d->priv;
    int rc = slot_gate(s, 1);

    return rc != 0 ? rc : blk_write(&s->ram, lba, n, buf);
}

static int slot_sync(blk_dev_t *d)
{
    return slot_gate(d->priv, 0);
}

/* Puts card (a RAM image) in the slot, present, with no removal planned. */
static void insert(void *card)
{
    CHECK_EQ(blk_ram_init(&tape.ram, card, DISK_SIZE, 0), 0);
    tape.dev = tape.ram;
    tape.dev.name = "tape";
    tape.dev.flags |= BLK_FLAG_REMOVABLE;
    tape.dev.read = slot_read;
    tape.dev.write = slot_write;
    tape.dev.sync = slot_sync;
    tape.dev.priv = &tape;
    tape.calls = tape.calls_out = tape.writes = 0;
    tape.remove_at = -1;
    tape.present = 1;
}

static void pattern(unsigned char *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(i * 7 + seed);
    }
}

/* Formats card and writes the files every test expects. */
static void make_card(uint64_t *card, const char *tag, unsigned seed)
{
    static unsigned char big[BIG_SIZE];
    blk_dev_t ram;

    memset(card, 0, DISK_SIZE);
    CHECK_EQ(blk_ram_init(&ram, card, DISK_SIZE, 0), 0);
    CHECK_EQ(fs_mkfs(&ram, NULL), FS_OK);
    CHECK_EQ(fs_mount("mk", &ram), FS_OK);
    CHECK_EQ(fs_mkdir("mk:/GAMES"), FS_OK);
    CHECK_EQ(fs_write_file_atomic("mk:/NAME.TXT", tag, strlen(tag)), FS_OK);
    pattern(big, sizeof(big), seed);
    CHECK_EQ(fs_write_file_atomic("mk:/GAMES/BIG.BIN", big, sizeof(big)), FS_OK);
    CHECK_EQ(fs_write_file_atomic("mk:/SAVE.DAT", "old save", 8), FS_OK);
    if (seed == 1) {
        CHECK_EQ(fs_write_file_atomic("mk:/ONLY_A.TXT", "a", 1), FS_OK);
    }
    CHECK_EQ(fs_unmount("mk"), FS_OK);
}

static void setup(void)
{
    static int done;

    fs_unmount("boot");
    fs_eject("tape");
    if (!done) {
        make_card(tmpl_a, "card A", 1);
        make_card(tmpl_b, "card B", 2);
        make_card(boot_words, "boot", 3);
        done = 1;
    }
    memcpy(card_a, tmpl_a, DISK_SIZE);
    memcpy(card_b, tmpl_b, DISK_SIZE);
    CHECK_EQ(blk_ram_init(&boot_dev, boot_words, DISK_SIZE, 0), 0);
    CHECK_EQ(fs_mount("boot", &boot_dev), FS_OK);
    insert(card_a);
}

static void expect_file(const char *path, const char *want)
{
    char back[64];
    size_t n = 0;

    CHECK_EQ(fs_read_file(path, back, sizeof(back), &n), FS_OK);
    CHECK_EQ(n, strlen(want));
    CHECK_MEM(back, want, n);
}

TEST(two_volumes_same_names)
{
    fs_stat_t st;

    setup();
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    expect_file("boot:/NAME.TXT", "boot");
    expect_file("tape:/NAME.TXT", "card A");
    expect_file("/NAME.TXT", "boot"); /* the default volume is the first mounted */
    CHECK_EQ(fs_write_file_atomic("tape:/NEW.TXT", "on tape", 7), FS_OK);
    CHECK_EQ(fs_stat("boot:/NEW.TXT", &st), FS_ERR_NOENT);
    CHECK_EQ(fs_rename("tape:/NEW.TXT", "boot:/NEW.TXT"), FS_ERR_INVAL);
    CHECK_EQ(fs_mount("third", &tape.dev), FS_ERR_MOUNTED);
    CHECK_EQ(fs_unmount("tape"), FS_OK);
    expect_file("NAME.TXT", "boot");
}

/*
 * Every step of a session on the tape. With the card pulled at device call
 * k, each step before the pull succeeds, and the step that meets the pull
 * and every one after it returns FS_ERR_GONE. The device sees at most one
 * call once the card is out, and the boot volume keeps working.
 */
enum { S_OPEN, S_READ, S_SEEK, S_READ2, S_TELL, S_SIZE, S_STAT, S_OPENDIR, S_READDIR, S_WOPEN,
       S_WRITE, S_ATOMIC, S_PROBE, S_STATVFS, S_CLOSEDIR, S_WCLOSE, S_CLOSE, S_COUNT };

static int session(int writable, int *rcs)
{
    static unsigned char buf[BIG_SIZE];
    fs_file_t f, w;
    fs_dir_t d;
    fs_stat_t st;
    fs_statvfs_t vf;
    int i, rc;

    for (i = 0; i < S_COUNT; i++) {
        rcs[i] = FS_OK;
    }
    rcs[S_OPEN] = fs_open(&f, "tape:/GAMES/BIG.BIN", FS_READ);
    rcs[S_READ] = fs_read(&f, buf, 100);
    rcs[S_SEEK] = fs_seek(&f, 4000, FS_SEEK_SET);
    rcs[S_READ2] = fs_read(&f, buf, 1000);
    rcs[S_TELL] = (int)fs_tell(&f);
    rcs[S_SIZE] = (int)fs_size(&f);
    rcs[S_STAT] = fs_stat("tape:/NAME.TXT", &st);
    rcs[S_OPENDIR] = fs_opendir(&d, "tape:/");
    do {
        rc = fs_readdir(&d, &st);
    } while (rc == 1);
    rcs[S_READDIR] = rc;
    if (writable) {
        rcs[S_WOPEN] = fs_open(&w, "tape:/LOG.TXT", FS_WRITE | FS_CREATE);
        rcs[S_WRITE] = fs_write(&w, buf, 3000);
        rcs[S_ATOMIC] = fs_write_file_atomic("tape:/SAVE.DAT", "new save!", 9);
    }
    rcs[S_PROBE] = fs_probe("tape");
    rcs[S_STATVFS] = fs_statvfs("tape", &vf);
    rcs[S_CLOSEDIR] = fs_closedir(&d);
    if (writable) {
        rcs[S_WCLOSE] = fs_close(&w);
    }
    rcs[S_CLOSE] = fs_close(&f);
    /* Closed handles stay closed, whatever happened. */
    CHECK_EQ(fs_read(&f, buf, 1), FS_ERR_BADF);
    CHECK_EQ(fs_readdir(&d, &st), FS_ERR_BADF);
    return (int)tape.calls;
}

static int ok_value(int step, int rc)
{
    switch (step) {
    case S_READ:    return rc == 100;
    case S_READ2:   return rc == 1000;
    case S_TELL:    return rc == 5000;
    case S_SIZE:    return rc == (int)BIG_SIZE;
    case S_READDIR: return rc == 0;
    case S_WRITE:   return rc == 3000;
    default:        return rc == FS_OK;
    }
}

static int skipped(int writable, int i)
{
    return !writable && (i == S_WOPEN || i == S_WRITE || i == S_ATOMIC || i == S_WCLOSE);
}

/* 1 when step i uses a handle whose open failed: that handle is not open. */
static int never_opened(int i, const int *rcs)
{
    switch (i) {
    case S_READ: case S_SEEK: case S_READ2: case S_TELL: case S_SIZE: case S_CLOSE:
        return rcs[S_OPEN] != FS_OK;
    case S_READDIR: case S_CLOSEDIR:
        return rcs[S_OPENDIR] != FS_OK;
    case S_WRITE: case S_WCLOSE:
        return rcs[S_WOPEN] != FS_OK;
    default:
        return 0;
    }
}

static void sweep(int writable)
{
    int rcs[S_COUNT];
    int total, k, i, gone;

    setup();
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    tape.calls = 0;
    total = session(writable, rcs);
    for (i = 0; i < S_COUNT; i++) {
        if (!skipped(writable, i) && !ok_value(i, rcs[i])) {
            RC_FAIL("no pull: step %d rc=%d", i, rcs[i]);
        }
    }
    CHECK_EQ(fs_unmount("tape"), FS_OK);
    for (k = 0; k <= total; k++) {
        uint32_t changes;

        setup();
        CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
        changes = fs_changes();
        tape.calls = 0;
        tape.remove_at = k;
        session(writable, rcs);
        gone = 0;
        for (i = 0; i < S_COUNT; i++) {
            if (skipped(writable, i)) {
                continue;
            }
            if (gone && never_opened(i, rcs)) {
                CHECK_EQ(rcs[i], FS_ERR_BADF);
            } else if (gone) {
                CHECK_EQ(rcs[i], FS_ERR_GONE);
            } else if (rcs[i] == FS_ERR_GONE) {
                gone = 1;
            } else if (!ok_value(i, rcs[i])) {
                RC_FAIL("k=%d step %d rc=%d", k, i, rcs[i]);
            }
        }
        CHECK(gone || k == total);
        CHECK(tape.calls_out <= 1);
        CHECK_EQ(fs_changes(), changes + (gone ? 1u : 0u));
        expect_file("boot:/NAME.TXT", "boot");

        /* Unmount does no I/O on a gone card, then a new card shows its own files. */
        tape.calls = 0;
        CHECK_EQ(fs_unmount("tape"), FS_OK);
        CHECK(gone ? tape.calls == 0 : 1);
        insert(card_b);
        CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
        expect_file("tape:/NAME.TXT", "card B");
        CHECK_EQ(fs_stat("tape:/ONLY_A.TXT", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
        CHECK_EQ(fs_unmount("tape"), FS_OK);
    }
}

TEST(pulled_during_read_session)
{
    sweep(0);
}

TEST(pulled_during_write_session)
{
    sweep(1);
}

/* A handle from before the pull never reads the new card's data, nor stale sizes. */
TEST(stale_handle_after_new_card)
{
    unsigned char buf[16];
    fs_file_t f;
    fs_dir_t d;
    fs_stat_t st;

    setup();
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    CHECK_EQ(fs_open(&f, "tape:/GAMES/BIG.BIN", FS_READ), FS_OK);
    CHECK_EQ(fs_read(&f, buf, sizeof(buf)), (int)sizeof(buf));
    CHECK_EQ(fs_opendir(&d, "tape:/GAMES"), FS_OK);
    tape.present = 0;
    /* The next bytes sit in the handle's cached sector: no device call is needed. */
    CHECK_EQ(fs_read(&f, buf, sizeof(buf)), (int)sizeof(buf));
    CHECK_EQ(fs_probe("tape"), FS_ERR_GONE);
    CHECK_EQ(fs_read(&f, buf, sizeof(buf)), FS_ERR_GONE);
    CHECK_EQ(fs_size(&f), FS_ERR_GONE);
    CHECK_EQ(fs_tell(&f), FS_ERR_GONE);
    CHECK_EQ(fs_seek(&f, 0, FS_SEEK_SET), FS_ERR_GONE);
    CHECK_EQ(fs_readdir(&d, &st), FS_ERR_GONE);
    CHECK_EQ(fs_stat("tape:/NAME.TXT", &st), FS_ERR_GONE);
    CHECK_EQ(fs_chdir("tape:/GAMES"), FS_ERR_GONE);
    CHECK_EQ(fs_unmount("tape"), FS_OK);
    insert(card_b);
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    CHECK_EQ(fs_read(&f, buf, sizeof(buf)), FS_ERR_BADF);
    CHECK_EQ(fs_size(&f), FS_ERR_BADF);
    CHECK_EQ(fs_tell(&f), FS_ERR_BADF);
    CHECK_EQ(fs_readdir(&d, &st), FS_ERR_BADF);
    CHECK_EQ(fs_close(&f), FS_ERR_BADF);
    CHECK_EQ(fs_closedir(&d), FS_ERR_BADF);
    expect_file("tape:/NAME.TXT", "card B");
    CHECK_EQ(fs_unmount("tape"), FS_OK);
}

/* 1000 inserts and pulls with open handles: no slot is lost. */
TEST(thousand_cycles)
{
    unsigned char buf[32];
    fs_file_t f;
    fs_dir_t d;
    int i;

    setup();
    for (i = 0; i < 1000; i++) {
        insert((i & 1) != 0 ? card_b : card_a);
        CHECK_EQ(fs_mount_flags("tape", &tape.dev, FS_MOUNT_RDONLY), FS_OK);
        CHECK_EQ(fs_open(&f, "tape:/GAMES/BIG.BIN", FS_READ), FS_OK);
        CHECK_EQ(fs_opendir(&d, "tape:/"), FS_OK);
        CHECK_EQ(fs_seek(&f, (int64_t)(i * 5) % BIG_SIZE, FS_SEEK_SET), FS_OK);
        tape.remove_at = (long)tape.calls + (i % 3);
        while (fs_read(&f, buf, sizeof(buf)) > 0) {
        }
        /* The pull may come after the last read: the probe notices it. */
        tape.present = 0;
        CHECK_EQ(fs_probe("tape"), FS_ERR_GONE);
        CHECK_EQ(fs_read(&f, buf, sizeof(buf)), FS_ERR_GONE);
        CHECK_EQ(fs_close(&f), FS_ERR_GONE);
        CHECK_EQ(fs_closedir(&d), FS_ERR_GONE);
        if ((i & 1) != 0) {
            CHECK_EQ(fs_unmount("tape"), FS_OK);
        } else {
            CHECK_EQ(fs_eject("tape"), FS_OK);
        }
    }
    insert(card_a);
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    expect_file("tape:/NAME.TXT", "card A");
    expect_file("boot:/NAME.TXT", "boot");
    CHECK_EQ(fs_unmount("tape"), FS_OK);
}

TEST(read_only_mount)
{
    char back[16];
    size_t n;
    fs_file_t f;

    setup();
    CHECK_EQ(fs_mount_flags("tape", &tape.dev, 0x80u), FS_ERR_INVAL);
    CHECK_EQ(fs_mount_flags("tape", &tape.dev, FS_MOUNT_RDONLY), FS_OK);
    tape.writes = 0;
    CHECK_EQ(fs_open(&f, "tape:/NAME.TXT", FS_READ | FS_WRITE), FS_ERR_RO);
    CHECK_EQ(fs_open(&f, "tape:/X.TXT", FS_WRITE | FS_CREATE), FS_ERR_RO);
    CHECK_EQ(fs_mkdir("tape:/D"), FS_ERR_RO);
    CHECK_EQ(fs_unlink("tape:/NAME.TXT"), FS_ERR_RO);
    CHECK_EQ(fs_rename("tape:/NAME.TXT", "tape:/N2.TXT"), FS_ERR_RO);
    CHECK_EQ(fs_write_file_atomic("tape:/SAVE.DAT", "x", 1), FS_ERR_RO);
    CHECK_EQ(fs_read_file_atomic("tape:/SAVE.DAT", back, sizeof(back), &n), FS_OK);
    CHECK_EQ(n, 8);
    CHECK_EQ(tape.writes, 0);
    CHECK_EQ(fs_unmount("tape"), FS_OK);
    CHECK_EQ(tape.writes, 0);
    /* The same card mounted writable again writes. */
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    CHECK_EQ(fs_mkdir("tape:/D"), FS_OK);
    CHECK(tape.writes > 0);
    CHECK_EQ(fs_unmount("tape"), FS_OK);
}

/*
 * A writable tape pulled at every device call of fs_write_file_atomic(),
 * then put back as it was. After fs_atomic_recover() the file holds the
 * complete old or the complete new content.
 */
TEST(pulled_during_atomic_write)
{
    char back[16];
    size_t n;
    int total, k;

    setup();
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    tape.calls = 0;
    CHECK_EQ(fs_write_file_atomic("tape:/SAVE.DAT", "new save!", 9), FS_OK);
    total = (int)tape.calls;
    CHECK_EQ(fs_unmount("tape"), FS_OK);
    for (k = 0; k < total; k++) {
        int rc;

        setup();
        CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
        tape.calls = 0;
        tape.remove_at = k;
        rc = fs_write_file_atomic("tape:/SAVE.DAT", "new save!", 9);
        CHECK_EQ(rc, FS_ERR_GONE);
        CHECK_EQ(fs_unmount("tape"), FS_OK);
        insert(card_a); /* the same card, as the pull left it */
        CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
        CHECK_EQ(fs_atomic_recover("tape:/SAVE.DAT"), FS_OK);
        CHECK_EQ(fs_read_file("tape:/SAVE.DAT", back, sizeof(back), &n), FS_OK);
        CHECK(n == 8 || n == 9);
        CHECK_MEM(back, n == 8 ? "old save" : "new save!", n);
        CHECK_EQ(fs_stat("tape:/~SAVE.DAT", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
        CHECK_EQ(fs_stat("tape:/#SAVE.DAT", &(fs_stat_t){ 0 }), FS_ERR_NOENT);
        CHECK_EQ(fs_unmount("tape"), FS_OK);
    }
}

/* Eject drops a live volume with no I/O; the counter tracks every change. */
TEST(eject_and_changes)
{
    fs_file_t f;
    uint32_t c0;

    setup();
    c0 = fs_changes();
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    CHECK_EQ(fs_changes(), c0 + 1);
    CHECK_EQ(fs_probe("tape"), FS_OK);
    CHECK_EQ(fs_open(&f, "tape:/LOG.TXT", FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_write(&f, "abc", 3), 3);
    tape.calls = 0;
    CHECK_EQ(fs_eject("tape"), FS_OK);
    CHECK_EQ(tape.calls, 0);
    CHECK_EQ(fs_changes(), c0 + 2);
    CHECK_EQ(fs_write(&f, "abc", 3), FS_ERR_BADF);
    CHECK_EQ(fs_close(&f), FS_ERR_BADF);
    CHECK_EQ(fs_eject("tape"), FS_ERR_NOVOL);
    CHECK_EQ(fs_probe("tape"), FS_ERR_NOVOL);
    CHECK_EQ(fs_stat("tape:/NAME.TXT", &(fs_stat_t){ 0 }), FS_ERR_NOVOL);
    /* A failed mount of a missing card leaves the slot free. */
    tape.present = 0;
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_ERR_NOTREADY);
    CHECK_EQ(fs_changes(), c0 + 2);
    tape.present = 1;
    CHECK_EQ(fs_mount("tape", &tape.dev), FS_OK);
    CHECK_EQ(fs_unmount("tape"), FS_OK);
    CHECK_STR(fs_strerror(FS_ERR_GONE), "medium removed");
}

/*
 * Slot state after a failed operation. A device that fails in one way on
 * demand: no medium, an I/O error, or a blank card with no file system.
 * Each operation that can fail is failed in each way, on slot 0 or with
 * slot 0 held so it lands on slot 1. Then the same checks run as on a
 * fresh system, and every result must match.
 */
enum { F_NONE, F_NOMEDIA, F_IO, F_BLANK, F_COUNT };
enum { OP_MOUNT, OP_MOUNT_RO, OP_MKFS, OP_PROBE, OP_COUNT };

typedef struct {
    blk_dev_t dev;
    blk_dev_t ram;
    int fault;
} faulty_t;

static uint64_t disk_x[DISK_SIZE / 8], disk_y[DISK_SIZE / 8];
static faulty_t dev_x, dev_y;

static int faulty_rc(blk_dev_t *d)
{
    faulty_t *f = d->priv;

    return f->fault == F_NOMEDIA ? BLK_ERR_NOMEDIA : f->fault == F_IO ? BLK_ERR_IO : 0;
}

static int faulty_read(blk_dev_t *d, uint64_t lba, uint32_t n, void *buf)
{
    int rc = faulty_rc(d);

    return rc != 0 ? rc : blk_read(&((faulty_t *)d->priv)->ram, lba, n, buf);
}

static int faulty_write(blk_dev_t *d, uint64_t lba, uint32_t n, const void *buf)
{
    int rc = faulty_rc(d);

    return rc != 0 ? rc : blk_write(&((faulty_t *)d->priv)->ram, lba, n, buf);
}

static int faulty_sync(blk_dev_t *d)
{
    return faulty_rc(d);
}

static void faulty_init(faulty_t *f, uint64_t *disk, const char *name)
{
    CHECK_EQ(blk_ram_init(&f->ram, disk, DISK_SIZE, 0), 0);
    f->dev = f->ram;
    f->dev.name = name;
    f->dev.flags |= BLK_FLAG_REMOVABLE;
    f->dev.read = faulty_read;
    f->dev.write = faulty_write;
    f->dev.sync = faulty_sync;
    f->dev.priv = f;
    f->fault = F_NONE;
}

/* x holds a blank card, y holds card A, nothing is mounted. */
static void fresh_system(void)
{
    fs_unmount("boot");
    fs_eject("tape");
    fs_eject("x");
    fs_eject("y");
    fs_eject("hold");
    memset(disk_x, 0, DISK_SIZE);
    memcpy(disk_y, tmpl_a, DISK_SIZE);
    faulty_init(&dev_x, disk_x, "x");
    faulty_init(&dev_y, disk_y, "y");
}

#define CHECKS 26

/* From src/fs/fs.c in host builds: no field of a free slot is left set. */
int fs_host_free_slots_clean(void);

/*
 * Every operation on x and y, each result in out. It formats a device in
 * slot 0 and in slot 1 before any mount there, because a mount sets the
 * flags itself and would hide a flag left over. Ends with nothing mounted.
 */
static void run_checks(int *out)
{
    fs_statvfs_t vf;
    int n = 0;

    out[n++] = fs_mkfs(&dev_x.dev, NULL);                    /* slot 0 */
    out[n++] = fs_mount_flags("x", &dev_x.dev, FS_MOUNT_RDONLY);
    out[n++] = fs_mkdir("x:/D");
    out[n++] = fs_mkfs(&dev_y.dev, NULL);                    /* slot 1 */
    out[n++] = fs_mount("y", &dev_y.dev);
    out[n++] = fs_stat("y:/NAME.TXT", &(fs_stat_t){ 0 });   /* formatted away */
    out[n++] = fs_unmount("y");
    out[n++] = fs_probe("x");
    out[n++] = fs_unmount("x");
    out[n++] = fs_mount("x", &dev_x.dev);
    out[n++] = fs_write_file_atomic("x:/A.TXT", "abc", 3);
    out[n++] = fs_write_file_atomic("/B.TXT", "def", 3);    /* the default volume */
    out[n++] = fs_mount("y", &dev_y.dev);
    out[n++] = fs_statvfs("y", &vf);
    out[n++] = fs_mkdir("y:/E");
    out[n++] = fs_stat("/B.TXT", &(fs_stat_t){ 0 });
    out[n++] = fs_eject("x");
    out[n++] = fs_stat("/E", &(fs_stat_t){ 0 });            /* now on y */
    out[n++] = fs_unmount("y");
    out[n++] = fs_mount_flags("y", &dev_y.dev, FS_MOUNT_RDONLY);
    out[n++] = fs_stat("y:/E", &(fs_stat_t){ 0 });
    out[n++] = fs_unmount("y");
    out[n++] = fs_probe("x");
    memset(disk_x, 0, DISK_SIZE);
    out[n++] = fs_mount("x", &dev_x.dev);                    /* blank: no file system */
    out[n++] = fs_mkfs(&dev_x.dev, NULL);
    out[n++] = fs_eject("x");
    CHECK(fs_host_free_slots_clean());
    CHECK_EQ(n, CHECKS);
}

/* Runs op on x with the fault set. Leaves nothing mounted on x. */
static int failed_op(int op, int fault)
{
    int rc;

    if (fault != F_BLANK) {
        CHECK_EQ(fs_mkfs(&dev_x.dev, NULL), FS_OK);
    }
    if (op == OP_PROBE) {
        /* A blank card fails the mount first: the probe then finds no volume. */
        CHECK_EQ(fs_mount("x", &dev_x.dev), fault == F_BLANK ? FS_ERR_NOFS : FS_OK);
    }
    dev_x.fault = fault == F_BLANK ? F_NONE : fault;
    switch (op) {
    case OP_MOUNT:    rc = fs_mount("x", &dev_x.dev); break;
    case OP_MOUNT_RO: rc = fs_mount_flags("x", &dev_x.dev, FS_MOUNT_RDONLY); break;
    case OP_MKFS:     rc = fs_mkfs(&dev_x.dev, NULL); break;
    default:          rc = fs_probe("x"); break;
    }
    dev_x.fault = F_NONE;
    if (op != OP_PROBE) {
        CHECK(fs_host_free_slots_clean());
    }
    fs_unmount("x");
    CHECK(fs_host_free_slots_clean());
    memset(disk_x, 0, DISK_SIZE);
    return rc;
}

TEST(failed_operation_leaves_clean_slot)
{
    static const int fresh_rc[CHECKS] = {
        FS_OK, FS_OK, FS_ERR_RO, FS_OK, FS_OK, FS_ERR_NOENT, FS_OK, FS_OK, FS_OK, FS_OK,
        FS_OK, FS_OK, FS_OK, FS_OK, FS_OK, FS_OK, FS_OK, FS_OK, FS_OK, FS_OK,
        FS_OK, FS_OK, FS_ERR_NOVOL, FS_ERR_NOFS, FS_OK, FS_ERR_NOVOL,
    };
    int base[CHECKS], got[CHECKS];
    int op, fault, hold, i, rc, failures = 0;

    setup(); /* builds the card templates */
    fresh_system();
    run_checks(base);
    CHECK_MEM(base, fresh_rc, sizeof(base));
    for (hold = 0; hold < 2; hold++) {
        for (op = 0; op < OP_COUNT; op++) {
            for (fault = F_NOMEDIA; fault < F_COUNT; fault++) {
                fresh_system();
                if (hold) {
                    CHECK_EQ(fs_mount("hold", &boot_dev), FS_OK);
                }
                rc = failed_op(op, fault);
                failures += rc != FS_OK;
                if (hold) {
                    CHECK_EQ(fs_unmount("hold"), FS_OK);
                }
                run_checks(got);
                for (i = 0; i < CHECKS; i++) {
                    if (got[i] != base[i]) {
                        RC_FAIL("hold=%d op=%d fault=%d rc=%d: check %d gave %d, fresh %d",
                                hold, op, fault, rc, i, got[i], base[i]);
                    }
                }
            }
        }
    }
    /* Every pair fails except mkfs on a blank card. */
    CHECK_EQ(failures, 2 * (OP_COUNT * (F_COUNT - 1) - 1));
}
