/* File API on RAM devices: files, directories, names, volumes, misuse. */
#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/fs.h>

#include "blk_file.h"
#include "rc_test.h"

#define DISK_SIZE (1024u * 1024u)

static uint64_t disk_a_words[DISK_SIZE / 8];
static uint64_t disk_b_words[DISK_SIZE / 8];
static unsigned char *const disk_a = (unsigned char *)disk_a_words;
static unsigned char *const disk_b = (unsigned char *)disk_b_words;
static blk_dev_t dev_a, dev_b;

static void reset(void)
{
    fs_unmount("a");
    fs_unmount("b");
    fs_unmount("c");
}

/* Formats disk_a and mounts it as "a", the default volume. */
static void setup(void)
{
    reset();
    memset(disk_a, 0, DISK_SIZE);
    CHECK_EQ(blk_ram_init(&dev_a, disk_a, DISK_SIZE, 0), 0);
    CHECK_EQ(fs_mkfs(&dev_a, NULL), FS_OK);
    CHECK_EQ(fs_mount("a", &dev_a), FS_OK);
}

static void put(const char *path, const char *text)
{
    fs_file_t f;

    CHECK_EQ(fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC), FS_OK);
    CHECK_EQ(fs_write(&f, text, strlen(text)), strlen(text));
    CHECK_EQ(fs_close(&f), FS_OK);
}

/* Checks that path holds exactly text. */
static void expect(const char *path, const char *text)
{
    char buf[512];
    size_t n = 0;

    CHECK_EQ(fs_read_file(path, buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, strlen(text));
    CHECK_MEM(buf, text, strlen(text));
}

TEST(mount_and_statvfs)
{
    fs_statvfs_t before, after;
    fs_file_t f;
    static char data[10000];

    setup();
    CHECK_EQ(fs_statvfs("a", &before), FS_OK);
    CHECK_EQ(before.fat_type, 12);
    CHECK(before.total_bytes > 900u * 1024u && before.total_bytes <= DISK_SIZE);
    CHECK_EQ(before.free_bytes, before.total_bytes);
    CHECK(before.cluster_bytes >= 512);
    memset(data, 'x', sizeof(data));
    CHECK_EQ(fs_open(&f, "a:/DATA.BIN", FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_write(&f, data, sizeof(data)), sizeof(data));
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_statvfs("a", &after), FS_OK);
    CHECK_EQ(after.free_bytes, before.free_bytes -
             (sizeof(data) + before.cluster_bytes - 1) / before.cluster_bytes * before.cluster_bytes);
    CHECK_EQ(fs_unlink("/DATA.BIN"), FS_OK);
    CHECK_EQ(fs_statvfs("a", &after), FS_OK);
    CHECK_EQ(after.free_bytes, before.free_bytes);
    CHECK_EQ(fs_statvfs("zz", &after), FS_ERR_NOVOL);
    CHECK_EQ(fs_statvfs("a", rc_launder(NULL)), FS_ERR_INVAL);
    reset();
}

TEST(mkfs_formats)
{
    static const struct {
        unsigned fmt;
        size_t size;
        unsigned type;
    } cases[] = {
        { FS_MKFS_ANY, 1u << 20, 12 },
        { FS_MKFS_FAT, 16u << 20, 16 },
        { FS_MKFS_FAT32, 64u << 20, 32 },
    };
    size_t i;

    reset();
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned char *buf = calloc(1, cases[i].size);
        fs_mkfs_opts_t o = { cases[i].fmt, 0 };
        fs_statvfs_t st;
        blk_dev_t d;

        CHECK(buf != NULL);
        CHECK_EQ(blk_ram_init(&d, buf, cases[i].size, 0), 0);
        CHECK_EQ(fs_mkfs(&d, &o), FS_OK);
        CHECK_EQ(fs_mount("c", &d), FS_OK);
        CHECK_EQ(fs_statvfs("c", &st), FS_OK);
        CHECK_EQ(st.fat_type, cases[i].type);
        put("c:/F.TXT", "fat");
        expect("c:/F.TXT", "fat");
        CHECK_EQ(fs_unmount("c"), FS_OK);
        free(buf);
    }
    {
        fs_mkfs_opts_t o = { 7, 0 };

        CHECK_EQ(blk_ram_init(&dev_a, disk_a, DISK_SIZE, 0), 0);
        CHECK_EQ(fs_mkfs(&dev_a, &o), FS_ERR_INVAL);
        o.fmt = FS_MKFS_FAT32; /* 1 MiB is too small for FAT32 */
        CHECK_EQ(fs_mkfs(&dev_a, &o), FS_ERR_MKFS);
        CHECK_EQ(fs_mkfs(rc_launder(NULL), NULL), FS_ERR_INVAL);
    }
}

TEST(write_read_back)
{
    fs_file_t f;
    char buf[64];

    setup();
    CHECK_EQ(fs_open(&f, "a:/HELLO.TXT", FS_READ | FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_write(&f, "hello, world", 12), 12);
    CHECK_EQ(fs_tell(&f), 12);
    CHECK_EQ(fs_size(&f), 12);
    CHECK_EQ(fs_seek(&f, 0, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_read(&f, buf, sizeof(buf)), 12);
    CHECK_MEM(buf, "hello, world", 12);
    CHECK_EQ(fs_read(&f, buf, sizeof(buf)), 0);
    CHECK_EQ(fs_sync(&f), FS_OK);
    CHECK_EQ(fs_close(&f), FS_OK);
    expect("/HELLO.TXT", "hello, world");
    expect("hello.txt", "hello, world");

    /* Read-only and write-only handles. */
    CHECK_EQ(fs_open(&f, "/HELLO.TXT", FS_READ), FS_OK);
    CHECK_EQ(fs_write(&f, "x", 1), FS_ERR_DENIED);
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_open(&f, "/HELLO.TXT", FS_WRITE), FS_OK);
    CHECK_EQ(fs_read(&f, buf, 1), FS_ERR_DENIED);
    CHECK_EQ(fs_write(&f, "J", 1), 1);
    CHECK_EQ(fs_close(&f), FS_OK);
    expect("/HELLO.TXT", "Jello, world");
    reset();
}

TEST(append_trunc_excl)
{
    fs_file_t f;

    setup();
    put("/LOG.TXT", "one");
    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_READ | FS_WRITE | FS_APPEND), FS_OK);
    CHECK_EQ(fs_seek(&f, 0, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_write(&f, ",two", 4), 4);
    CHECK_EQ(fs_seek(&f, 1, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_write(&f, ",three", 6), 6);
    CHECK_EQ(fs_close(&f), FS_OK);
    expect("/LOG.TXT", "one,two,three");

    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_WRITE | FS_TRUNC), FS_OK);
    CHECK_EQ(fs_size(&f), 0);
    CHECK_EQ(fs_close(&f), FS_OK);
    expect("/LOG.TXT", "");
    CHECK_EQ(fs_open(&f, "/NEW.TXT", FS_WRITE | FS_TRUNC), FS_ERR_NOENT);

    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_WRITE | FS_CREATE | FS_EXCL), FS_ERR_EXIST);
    CHECK_EQ(fs_open(&f, "/NEW.TXT", FS_WRITE | FS_CREATE | FS_EXCL), FS_OK);
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_open(&f, "/NEW.TXT", FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_close(&f), FS_OK);

    /* Flag combinations that make no sense. */
    CHECK_EQ(fs_open(&f, "/LOG.TXT", 0), FS_ERR_INVAL);
    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_READ | FS_TRUNC), FS_ERR_INVAL);
    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_READ | FS_APPEND), FS_ERR_INVAL);
    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_READ | FS_CREATE), FS_ERR_INVAL);
    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_WRITE | FS_EXCL), FS_ERR_INVAL);
    CHECK_EQ(fs_open(&f, "/LOG.TXT", FS_READ | 0x100u), FS_ERR_INVAL);
    reset();
}

TEST(seek_and_sparse)
{
    fs_file_t f;
    unsigned char buf[3000];
    size_t i;

    setup();
    put("/S.BIN", "0123456789");
    CHECK_EQ(fs_open(&f, "/S.BIN", FS_READ | FS_WRITE), FS_OK);
    CHECK_EQ(fs_seek(&f, 4, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_seek(&f, 2, FS_SEEK_CUR), FS_OK);
    CHECK_EQ(fs_tell(&f), 6);
    CHECK_EQ(fs_seek(&f, -3, FS_SEEK_END), FS_OK);
    CHECK_EQ(fs_tell(&f), 7);
    CHECK_EQ(fs_read(&f, buf, 10), 3);
    CHECK_MEM(buf, "789", 3);
    CHECK_EQ(fs_seek(&f, -11, FS_SEEK_END), FS_ERR_INVAL);
    CHECK_EQ(fs_seek(&f, 0, 3), FS_ERR_INVAL);
    CHECK_EQ(fs_seek(&f, INT64_C(0x100000000), FS_SEEK_SET), FS_ERR_INVAL);

    /* Past the end of a writable file: the gap reads as zeros. */
    CHECK_EQ(fs_seek(&f, 2000, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_tell(&f), 2000);
    CHECK_EQ(fs_size(&f), 2000);
    CHECK_EQ(fs_write(&f, "END", 3), 3);
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_read_file("/S.BIN", buf, sizeof(buf), &i), FS_OK);
    CHECK_EQ(i, 2003);
    CHECK_MEM(buf, "0123456789", 10);
    for (i = 10; i < 2000; i++) {
        if (buf[i] != 0) {
            CHECK_EQ(buf[i], 0);
            break;
        }
    }
    CHECK_MEM(buf + 2000, "END", 3);

    /* Past the end of a read-only handle: stops at the end. */
    CHECK_EQ(fs_open(&f, "/S.BIN", FS_READ), FS_OK);
    CHECK_EQ(fs_seek(&f, 5000, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_tell(&f), 2003);
    CHECK_EQ(fs_read(&f, buf, 10), 0);
    CHECK_EQ(fs_size(&f), 2003);
    CHECK_EQ(fs_close(&f), FS_OK);
    reset();
}

TEST(directories)
{
    fs_stat_t st;
    fs_file_t f;

    setup();
    CHECK_EQ(fs_mkdir("/A"), FS_OK);
    CHECK_EQ(fs_mkdir("/A/B"), FS_OK);
    CHECK_EQ(fs_mkdir("/A/B/C"), FS_OK);
    CHECK_EQ(fs_mkdir("/A"), FS_ERR_EXIST);
    CHECK_EQ(fs_mkdir("/X/Y"), FS_ERR_NOENT);
    put("/A/B/C/DEEP.TXT", "deep");
    expect("a:/A/B/C/DEEP.TXT", "deep");
    CHECK_EQ(fs_stat("/A/B", &st), FS_OK);
    CHECK_EQ(st.is_dir, 1);
    CHECK((st.attr & FS_ATTR_DIR) != 0);
    CHECK_STR(st.name, "B");
    CHECK_EQ(fs_stat("/A/B/C/DEEP.TXT", &st), FS_OK);
    CHECK_EQ(st.is_dir, 0);
    CHECK_EQ(st.size, 4);
    CHECK_EQ(fs_stat("/", &st), FS_OK);
    CHECK_EQ(st.is_dir, 1);
    CHECK_EQ(fs_stat("a:/", &st), FS_OK);
    CHECK_EQ(st.is_dir, 1);

    /* Rename a file across directories, then a directory. */
    CHECK_EQ(fs_rename("/A/B/C/DEEP.TXT", "/A/MOVED.TXT"), FS_OK);
    CHECK_EQ(fs_stat("/A/B/C/DEEP.TXT", &st), FS_ERR_NOENT);
    expect("/A/MOVED.TXT", "deep");
    put("/A/OTHER.TXT", "o");
    CHECK_EQ(fs_rename("/A/OTHER.TXT", "/A/MOVED.TXT"), FS_ERR_EXIST);
    CHECK_EQ(fs_rename("/A/NONE.TXT", "/A/X.TXT"), FS_ERR_NOENT);
    CHECK_EQ(fs_rename("/A/B", "/BB"), FS_OK);
    CHECK_EQ(fs_stat("/BB/C", &st), FS_OK);

    /* Unlink. */
    CHECK_EQ(fs_unlink("/BB"), FS_ERR_DENIED);
    CHECK_EQ(fs_unlink("/BB/C"), FS_OK);
    CHECK_EQ(fs_unlink("/BB"), FS_OK);
    CHECK_EQ(fs_unlink("/BB"), FS_ERR_NOENT);
    CHECK_EQ(fs_unlink("/A/MOVED.TXT"), FS_OK);
    CHECK_EQ(fs_open(&f, "/A/MOVED.TXT", FS_READ), FS_ERR_NOENT);

    /* Relative paths and the current directory. */
    CHECK_EQ(fs_chdir("/A"), FS_OK);
    expect("OTHER.TXT", "o");
    expect("../A/./OTHER.TXT", "o");
    put("REL.TXT", "rel");
    expect("/A/REL.TXT", "rel");
    CHECK_EQ(fs_chdir("/NOPE"), FS_ERR_NOENT);
    CHECK_EQ(fs_chdir("a:/"), FS_OK);
    expect("A/REL.TXT", "rel");
    reset();
}

TEST(listing_order_and_long_names)
{
    static const char *const names[] = {
        "ZETA.TXT", "alpha.txt", "A long file name with spaces.text",
        "\xc3\x9c\x62\x65rl\xc3\xa4nge.txt", "SUB",
    };
    fs_dir_t d;
    fs_stat_t st;
    size_t i;
    int rc;

    setup();
    for (i = 0; i < 5; i++) {
        char path[128];

        snprintf(path, sizeof(path), "/%s", names[i]);
        if (i == 4) {
            CHECK_EQ(fs_mkdir(path), FS_OK);
        } else {
            put(path, names[i]);
        }
    }
    CHECK_EQ(fs_opendir(&d, "/"), FS_OK);
    for (i = 0; (rc = fs_readdir(&d, &st)) == 1; i++) {
        if (i < 5) {
            CHECK_STR(st.name, names[i]);
            CHECK_EQ(st.is_dir, i == 4);
            CHECK_EQ(st.size, i == 4 ? 0 : strlen(names[i]));
        }
    }
    CHECK_EQ(rc, 0);
    CHECK_EQ(i, 5);
    CHECK_EQ(fs_readdir(&d, &st), 0);
    CHECK_EQ(fs_closedir(&d), FS_OK);
    CHECK_EQ(fs_readdir(&d, &st), FS_ERR_BADF);
    CHECK_EQ(fs_closedir(&d), FS_ERR_BADF);

    /* Long names get an 8.3 alias; lookups are case-insensitive. */
    CHECK_EQ(fs_stat("/a LONG file NAME with spaces.TEXT", &st), FS_OK);
    CHECK_EQ(st.size, 33);
    CHECK_EQ(fs_stat("/A long file name with spaces.text", &st), FS_OK);
    CHECK_STR(st.name, "A long file name with spaces.text");
    CHECK_STR(st.short_name, "ALONGF~1.TEX");
    expect("/ALONGF~1.TEX", "A long file name with spaces.text");
    expect("/\xc3\x9c\x62\x65rl\xc3\xa4nge.txt", names[3]);

    /* A subdirectory lists without "." and "..". */
    put("/SUB/IN.TXT", "in");
    CHECK_EQ(fs_opendir(&d, "/SUB"), FS_OK);
    CHECK_EQ(fs_readdir(&d, &st), 1);
    CHECK_STR(st.name, "IN.TXT");
    CHECK_EQ(fs_readdir(&d, &st), 0);
    CHECK_EQ(fs_closedir(&d), FS_OK);
    CHECK_EQ(fs_opendir(&d, "/NOPE"), FS_ERR_NOENT);
    CHECK_EQ(fs_opendir(&d, "/SUB/IN.TXT"), FS_ERR_NOENT);
    CHECK_EQ(fs_readdir(&d, &st), FS_ERR_BADF);
    reset();
}

TEST(boot_protocol_names)
{
    fs_stat_t st;
    static unsigned char rom[40000];
    size_t i, n;
    static unsigned char back[40000];

    setup();
    for (i = 0; i < sizeof(rom); i++) {
        rom[i] = (unsigned char)(i * 7 + 3);
    }
    CHECK_EQ(fs_mkdir("/ROBCO"), FS_OK);
    CHECK_EQ(fs_mkdir("/PIPBOY"), FS_OK);
    put("/ROBCO/MFBOOT.SYS", "mfboot");
    CHECK_EQ(fs_write_file_atomic("/PIPBOY/ROM1.ROM", rom, sizeof(rom)), FS_OK);
    CHECK_EQ(fs_stat("/ROBCO/MFBOOT.SYS", &st), FS_OK);
    CHECK_STR(st.name, "MFBOOT.SYS");
    CHECK_EQ(fs_read_file("a:/PIPBOY/ROM1.ROM", back, sizeof(back), &n), FS_OK);
    CHECK_EQ(n, sizeof(rom));
    CHECK_MEM(back, rom, sizeof(rom));
    CHECK_EQ(fs_read_file("/pipboy/rom1.rom", back, 100, &n), FS_ERR_TOOBIG);
    CHECK_EQ(n, 100);
    CHECK_MEM(back, rom, 100);
    expect("/robco/mfboot.sys", "mfboot");
    CHECK_EQ(fs_stat("/PIPBOY/ROM1.TMP", &st), FS_ERR_NOENT);
    reset();
}

TEST(remount_keeps_data)
{
    setup();
    CHECK_EQ(fs_mkdir("/KEEP"), FS_OK);
    put("/KEEP/DATA.TXT", "persist");
    CHECK_EQ(fs_unmount("a"), FS_OK);
    CHECK_EQ(fs_unmount("a"), FS_ERR_NOVOL);
    CHECK_EQ(fs_stat("/KEEP", &(fs_stat_t){ 0 }), FS_ERR_NOVOL);
    CHECK_EQ(fs_mount("a", &dev_a), FS_OK);
    expect("/KEEP/DATA.TXT", "persist");
    reset();
}

TEST(two_volumes)
{
    char buf[32];
    size_t n;

    setup();
    memset(disk_b, 0, DISK_SIZE);
    CHECK_EQ(blk_ram_init(&dev_b, disk_b, DISK_SIZE, 0), 0);
    CHECK_EQ(fs_mkfs(&dev_b, NULL), FS_OK);
    CHECK_EQ(fs_mkfs(&dev_a, NULL), FS_ERR_MOUNTED);
    CHECK_EQ(fs_mount("a", &dev_b), FS_ERR_BUSY);  /* name in use */
    CHECK_EQ(fs_mount("b", &dev_b), FS_OK);
    CHECK_EQ(fs_mount("c", &dev_b), FS_ERR_MOUNTED);
    CHECK_EQ(fs_mkfs(&dev_b, NULL), FS_ERR_MOUNTED);
    {
        blk_dev_t other;

        CHECK_EQ(blk_ram_init(&other, disk_b, DISK_SIZE, 0), 0);
        CHECK_EQ(fs_mount("c", &other), FS_ERR_BUSY); /* no free slot */
    }

    put("a:/SAME.TXT", "on a");
    put("b:/SAME.TXT", "on b");
    expect("a:/SAME.TXT", "on a");
    expect("b:/SAME.TXT", "on b");
    expect("/SAME.TXT", "on a"); /* "a" was mounted first */
    CHECK_EQ(fs_rename("a:/SAME.TXT", "b:/OTHER.TXT"), FS_ERR_INVAL);
    CHECK_EQ(fs_chdir("b:/"), FS_OK);
    expect("/SAME.TXT", "on b");
    CHECK_EQ(fs_chdir("a:/"), FS_OK);
    CHECK_EQ(fs_unmount("a"), FS_OK);
    expect("/SAME.TXT", "on b");
    CHECK_EQ(fs_read_file("a:/SAME.TXT", buf, sizeof(buf), &n), FS_ERR_NOVOL);
    CHECK_EQ(fs_unmount("b"), FS_OK);
    CHECK_EQ(fs_read_file("/SAME.TXT", buf, sizeof(buf), &n), FS_ERR_NOVOL);
    reset();
}

/* One medium under two volume names would corrupt it. */
TEST(same_device_twice)
{
    setup();
    CHECK_EQ(fs_mount("b", &dev_a), FS_ERR_MOUNTED);
    CHECK_EQ(fs_mount("a", &dev_a), FS_ERR_BUSY); /* the name is checked first */
    CHECK_EQ(fs_mkfs(&dev_a, NULL), FS_ERR_MOUNTED);
    put("/A.TXT", "kept");
    CHECK_EQ(fs_unmount("a"), FS_OK);
    CHECK_EQ(fs_mount("b", &dev_a), FS_OK);
    expect("b:/A.TXT", "kept");
    CHECK_EQ(fs_mount("a", &dev_a), FS_ERR_MOUNTED);
    CHECK_STR(fs_strerror(FS_ERR_MOUNTED), "device already mounted");
    reset();
}

TEST(misuse)
{
    char long_path[400];
    char long_name[300];
    fs_file_t f;
    fs_stat_t st;
    unsigned char junk[4096];
    blk_dev_t d;

    setup();
    CHECK_EQ(fs_mkdir("/DIR"), FS_OK);
    CHECK_EQ(fs_open(&f, "/MISSING.TXT", FS_READ), FS_ERR_NOENT);
    CHECK_EQ(fs_open(&f, "/NODIR/MISSING.TXT", FS_READ), FS_ERR_NOENT);
    CHECK_EQ(fs_open(&f, "/DIR", FS_READ), FS_ERR_ISDIR);
    CHECK_EQ(fs_open(&f, "/DIR", FS_WRITE | FS_CREATE), FS_ERR_ISDIR);
    CHECK_EQ(fs_open(&f, "/", FS_READ), FS_ERR_ISDIR);
    CHECK_EQ(fs_open(&f, "zz:/A.TXT", FS_READ), FS_ERR_NOVOL);
    CHECK_EQ(fs_open(&f, "toolongname:/A.TXT", FS_READ), FS_ERR_NOVOL);
    CHECK_EQ(fs_open(&f, rc_launder(NULL), FS_READ), FS_ERR_INVAL);
    CHECK_EQ(fs_open(rc_launder(NULL), "/A.TXT", FS_READ), FS_ERR_INVAL);
    CHECK_EQ(fs_open(&f, "/BAD|NAME.TXT", FS_WRITE | FS_CREATE), FS_ERR_NAME);

    memset(long_path, 'A', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1] = '\0';
    CHECK_EQ(fs_open(&f, long_path, FS_READ), FS_ERR_NAMETOOLONG);
    CHECK_EQ(fs_stat(long_path, &st), FS_ERR_NAMETOOLONG);
    CHECK_EQ(fs_write_file_atomic(long_path, "x", 1), FS_ERR_NAMETOOLONG);
    long_path[FS_PATH_MAX - 1] = '\0'; /* fits with the "N:" prefix? no: 2 over */
    CHECK_EQ(fs_open(&f, long_path, FS_READ), FS_ERR_NAMETOOLONG);
    memset(long_name, 'b', sizeof(long_name));
    long_name[0] = '/';
    long_name[FS_PATH_MAX - 2] = '\0'; /* a 252-byte name fits */
    CHECK_EQ(fs_open(&f, long_name, FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_close(&f), FS_OK);

    /* Closed and never opened handles. */
    CHECK_EQ(fs_open(&f, "/A.TXT", FS_WRITE | FS_CREATE), FS_OK);
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_close(&f), FS_ERR_BADF);
    CHECK_EQ(fs_read(&f, junk, 1), FS_ERR_BADF);
    CHECK_EQ(fs_write(&f, junk, 1), FS_ERR_BADF);
    CHECK_EQ(fs_seek(&f, 0, FS_SEEK_SET), FS_ERR_BADF);
    CHECK_EQ(fs_tell(&f), FS_ERR_BADF);
    CHECK_EQ(fs_size(&f), FS_ERR_BADF);
    CHECK_EQ(fs_sync(&f), FS_ERR_BADF);
    memset(&f, 0xA5, sizeof(f));
    CHECK_EQ(fs_read(&f, junk, 1), FS_ERR_BADF);
    CHECK_EQ(fs_close(rc_launder(NULL)), FS_ERR_BADF);

    /* A handle whose volume went away. */
    CHECK_EQ(fs_open(&f, "/A.TXT", FS_READ), FS_OK);
    CHECK_EQ(fs_unmount("a"), FS_OK);
    CHECK_EQ(fs_read(&f, junk, 1), FS_ERR_BADF);
    CHECK_EQ(fs_mount("a", &dev_a), FS_OK);
    CHECK_EQ(fs_read(&f, junk, 1), FS_ERR_BADF);

    /* Bad mounts. */
    memset(junk, 0x11, sizeof(junk));
    CHECK_EQ(blk_ram_init(&d, junk, sizeof(junk), 0), 0);
    CHECK_EQ(fs_mount("j", &d), FS_ERR_NOFS);
    CHECK_EQ(fs_mount("", &d), FS_ERR_INVAL);
    CHECK_EQ(fs_mount("a:b", &d), FS_ERR_INVAL);
    CHECK_EQ(fs_mount("eightchr", &d), FS_ERR_INVAL);
    CHECK_EQ(fs_mount(rc_launder(NULL), &d), FS_ERR_INVAL);
    CHECK_EQ(fs_mount("j", rc_launder(NULL)), FS_ERR_INVAL);
    d.block_size = 4096;
    CHECK_EQ(fs_mount("j", &d), FS_ERR_INVAL);
    CHECK_EQ(fs_unmount("j"), FS_ERR_NOVOL);

    /* Atomic writes refuse names they cannot handle. */
    CHECK_EQ(fs_write_file_atomic("/~X.SAV", "x", 1), FS_ERR_NAME);
    CHECK_EQ(fs_write_file_atomic("/DIR/#X.SAV", "x", 1), FS_ERR_NAME);
    CHECK_EQ(fs_write_file_atomic("/DIR/..", "x", 1), FS_ERR_NAME);
    CHECK_EQ(fs_write_file_atomic("a:", "x", 1), FS_ERR_NAME);
    CHECK_EQ(fs_write_file_atomic("/DIR/", "x", 1), FS_ERR_NAME);
    CHECK_EQ(fs_write_file_atomic(rc_launder(NULL), "x", 1), FS_ERR_INVAL);
    CHECK_EQ(fs_write_file_atomic("/NOEXT", "noext", 5), FS_OK);
    expect("/NOEXT", "noext");
    reset();
}

TEST(strerror_names)
{
    int e;

    for (e = 0; e >= FS_ERR_MOUNTED; e--) {
        CHECK(strcmp(fs_strerror(e), "unknown error") != 0);
    }
    CHECK_STR(fs_strerror(FS_ERR_MOUNTED - 1), "unknown error");
    CHECK_STR(fs_strerror(1), "unknown error");
    CHECK_STR(fs_strerror(FS_ERR_NOSPC), "no space left");
}
