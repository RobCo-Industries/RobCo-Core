/*
 * The read-only file system flavour, ROBCO_FS_READONLY=1. Reads work and
 * every call that would write returns FS_ERR_RO. The image is
 * build/host/fs_readonly.img, made by fs_tool: a 1 MiB FAT volume with
 * /A/B.TXT holding a copy of tests/host/rc_test.h.
 */
#include <stdio.h>
#include <stdlib.h>

#include <robco/fs.h>

#include "blk_file.h"
#include "rc_test.h"

#define IMG  "build/host/fs_readonly.img"
#define SRC  "tests/host/rc_test.h"
#define MAXF 65536

static blk_dev_t dev;
static unsigned char want[MAXF];
static size_t want_len;
static unsigned char *img_before;
static long img_size;

static unsigned char *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc((size_t)*size + 1);
    if (p != NULL && fread(p, 1, (size_t)*size, f) != (size_t)*size) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

static void setup(void)
{
    long n = 0;
    unsigned char *src = slurp(SRC, &n);

    fs_unmount("ro");
    CHECK(src != NULL && n > 0 && n <= MAXF);
    if (src != NULL && n > 0 && n <= MAXF) {
        memcpy(want, src, (size_t)n);
        want_len = (size_t)n;
    }
    free(src);
    free(img_before);
    img_before = slurp(IMG, &img_size);
    CHECK(img_before != NULL);
    CHECK_EQ(blk_file_open(&dev, IMG, 0), 0);
    CHECK_EQ(fs_mount("ro", &dev), FS_OK);
}

static void teardown(void)
{
    long n = 0;
    unsigned char *after;

    CHECK_EQ(fs_unmount("ro"), FS_OK);
    blk_file_close(&dev);
    after = slurp(IMG, &n);
    CHECK(after != NULL && n == img_size);
    if (after != NULL && img_before != NULL && n == img_size) {
        CHECK_MEM(after, img_before, (size_t)n);
    }
    free(after);
}

TEST(mount_list_read)
{
    static unsigned char buf[MAXF];
    fs_dir_t d;
    fs_stat_t st;
    fs_file_t f;
    size_t n = 0;
    int count = 0;

    setup();
    CHECK_EQ(fs_opendir(&d, "ro:/"), FS_OK);
    while (fs_readdir(&d, &st) == 1) {
        CHECK_STR(st.name, "A");
        CHECK(st.is_dir);
        count++;
    }
    CHECK_EQ(count, 1);
    CHECK_EQ(fs_closedir(&d), FS_OK);
    CHECK_EQ(fs_stat("/A/B.TXT", &st), FS_OK);
    CHECK_EQ(st.size, want_len);

    CHECK_EQ(fs_read_file("/A/B.TXT", buf, sizeof(buf), &n), FS_OK);
    CHECK_EQ(n, want_len);
    CHECK_MEM(buf, want, want_len);

    CHECK_EQ(fs_open(&f, "/a/b.txt", FS_READ), FS_OK);
    CHECK_EQ(fs_size(&f), (int64_t)want_len);
    CHECK_EQ(fs_seek(&f, 10, FS_SEEK_SET), FS_OK);
    CHECK_EQ(fs_read(&f, buf, 5), 5);
    CHECK_MEM(buf, want + 10, 5);
    CHECK_EQ(fs_write(&f, "x", 1), FS_ERR_RO);
    CHECK_EQ(fs_sync(&f), FS_OK); /* nothing to write */
    CHECK_EQ(fs_close(&f), FS_OK);
    CHECK_EQ(fs_chdir("ro:/A"), FS_OK);
    CHECK_EQ(fs_stat("B.TXT", &st), FS_OK);
    teardown();
}

TEST(writes_return_ro)
{
    fs_file_t f;
    fs_statvfs_t sv;

    setup();
    CHECK_EQ(fs_open(&f, "/NEW.TXT", FS_WRITE | FS_CREATE), FS_ERR_RO);
    CHECK_EQ(fs_open(&f, "/A/B.TXT", FS_WRITE), FS_ERR_RO);
    CHECK_EQ(fs_open(&f, "/A/B.TXT", FS_READ | FS_WRITE | FS_APPEND), FS_ERR_RO);
    CHECK_EQ(fs_unlink("/A/B.TXT"), FS_ERR_RO);
    CHECK_EQ(fs_rename("/A/B.TXT", "/A/C.TXT"), FS_ERR_RO);
    CHECK_EQ(fs_mkdir("/D"), FS_ERR_RO);
    CHECK_EQ(fs_write_file_atomic("/A/B.TXT", "x", 1), FS_ERR_RO);
    sv.total_bytes = 1;
    CHECK_EQ(fs_statvfs("ro", &sv), FS_ERR_RO);
    CHECK_EQ(sv.total_bytes, 0u);
    CHECK_EQ(fs_statvfs("nope", &sv), FS_ERR_NOVOL);
    CHECK_EQ(fs_mkfs(&dev, NULL), FS_ERR_RO);
    CHECK_STR(fs_strerror(FS_ERR_RO), "read-only");
    teardown();
}
