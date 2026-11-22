/* Stage loader on a RAM disk with a FAT file system: every error code, guards around the slot. */
#include <stdint.h>

#include <robco/blk_ram.h>
#include <robco/crc32.h>
#include <robco/fs.h>
#include <robco/loader.h>
#include <robco/rimg.h>

#include "rc_test.h"

#define DISK_SIZE   (4u * 1024u * 1024u)
#define SLOT_BASE   0x00200000u
#define SLOT_SIZE   (64u * 1024u)
#define GUARD       4096u
#define FILL        0xA5u
#define MAX_FILE    (SLOT_SIZE + 2u * LOADER_CHUNK)
#define CRC_OFF     0x3Cu

static uint64_t disk_words[DISK_SIZE / 8];
static blk_dev_t dev;
static uint64_t arena_words[(GUARD + SLOT_SIZE + GUARD) / 8];
static uint8_t *const arena = (uint8_t *)arena_words;
static uint8_t *const slot_mem = (uint8_t *)arena_words + GUARD;
static uint8_t img[MAX_FILE];

static const rimg_expect_t boot_expect = { RIMG_MAGIC_BOOT, 0, 0, 2, 0 };

static loader_slot_t slot(void)
{
    loader_slot_t s = { SLOT_BASE, SLOT_SIZE, slot_mem };

    return s;
}

static void setup(void)
{
    fs_unmount("t");
    memset(disk_words, 0, sizeof(disk_words));
    CHECK_EQ(blk_ram_init(&dev, disk_words, DISK_SIZE, 0), 0);
    CHECK_EQ(fs_mkfs(&dev, NULL), FS_OK);
    CHECK_EQ(fs_mount("t", &dev), FS_OK);
    memset(arena, FILL, GUARD + SLOT_SIZE + GUARD);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void seal(uint32_t size)
{
    put32(img + CRC_OFF, 0);
    put32(img + CRC_OFF, crc32(img, size));
}

/* A sealed MFBootAgent image (BOOT 4) of `size` bytes for the test slot. */
static void build(uint32_t size)
{
    uint32_t i;

    memset(img, 0, sizeof(img));
    put32(img, 0xEA000016u); /* b 0x60 */
    memcpy(img + 0x20, RIMG_MAGIC_BOOT, 8);
    img[0x28] = 1;
    img[0x2A] = 64;
    put32(img + 0x30, SLOT_BASE);
    put32(img + 0x34, size);
    put32(img + 0x38, size + 256 <= SLOT_SIZE ? size + 256 : size);
    put32(img + 0x40, 2u << 16);
    put32(img + 0x48, RIMG_BOOT_AGENT);
    memcpy(img + 0x4C, "test", 4);
    for (i = 0x60; i < size; i++) {
        img[i] = (uint8_t)(i * 7u + (i >> 8));
    }
    seal(size);
}

static void write_file(const char *path, const void *data, uint32_t len)
{
    fs_file_t f;

    CHECK_EQ(fs_open(&f, path, FS_WRITE | FS_CREATE | FS_TRUNC), FS_OK);
    if (len > 0) {
        CHECK_EQ(fs_write(&f, data, len), len);
    }
    CHECK_EQ(fs_close(&f), FS_OK);
}

/* No byte before or after the slot changed. */
static void check_guards(void)
{
    uint32_t i;

    for (i = 0; i < GUARD; i++) {
        if (arena[i] != FILL || slot_mem[SLOT_SIZE + i] != FILL) {
            RC_FAIL("guard byte %u changed", (unsigned)i);
            return;
        }
    }
}

/* The slot was not written at all. */
static void check_slot_untouched(void)
{
    uint32_t i;

    check_guards();
    for (i = 0; i < SLOT_SIZE; i++) {
        if (slot_mem[i] != FILL) {
            RC_FAIL("slot byte %u written", (unsigned)i);
            return;
        }
    }
}

static int load(const char *path, loader_result_t *out)
{
    loader_slot_t s = slot();

    memset(arena, FILL, GUARD + SLOT_SIZE + GUARD);
    return loader_load(path, &s, &boot_expect, out);
}

/* Writes img[0..size) to path and expects `err` with no write into the slot. */
static void expect_header_err(const char *path, uint32_t size, int err)
{
    loader_result_t r;

    write_file(path, img, size);
    CHECK_EQ(load(path, &r), err);
    CHECK_EQ(r.err, err);
    CHECK(r.hdr == NULL);
    check_slot_untouched();
}

TEST(loads_image)
{
    loader_result_t r;
    uint32_t size = 3u * LOADER_CHUNK + 123u;

    setup();
    build(size);
    CHECK_EQ(fs_mkdir("t:/ROBCO"), FS_OK);
    write_file("t:/ROBCO/MFBOOT.SYS", img, size);
    CHECK_EQ(load("t:/ROBCO/MFBOOT.SYS", &r), RIMG_OK);
    CHECK_EQ(r.err, RIMG_OK);
    CHECK(r.hdr == (const rimg_header_t *)(slot_mem + 0x20));
    CHECK_EQ(r.entry, SLOT_BASE);
    CHECK_EQ(r.file_size, size);
    CHECK_EQ(r.fat_size, size);
    CHECK_EQ(r.mem_size, size + 256);
    CHECK_EQ(r.crc32, (uint32_t)img[CRC_OFF] | (uint32_t)img[CRC_OFF + 1] << 8 |
             (uint32_t)img[CRC_OFF + 2] << 16 | (uint32_t)img[CRC_OFF + 3] << 24);
    CHECK_STR(r.path, "t:/ROBCO/MFBOOT.SYS");
    CHECK_EQ(r.fallback, 0);
    CHECK_MEM(slot_mem, img, size);
    /* Bytes past file_size inside the slot stay as they were: no BSS clearing. */
    CHECK_EQ(slot_mem[size], FILL);
    check_guards();
    fs_unmount("t");
}

TEST(null_out_and_null_expect)
{
    loader_slot_t s = slot();

    setup();
    build(1000);
    write_file("t:/A.SYS", img, 1000);
    CHECK_EQ(loader_load("t:/A.SYS", &s, NULL, NULL), RIMG_OK);
    CHECK_MEM(slot_mem, img, 1000);
    CHECK_EQ(loader_load("t:/A.SYS", rc_launder(NULL), NULL, NULL), RIMG_E_IO);
    CHECK_EQ(loader_load(rc_launder(NULL), &s, NULL, NULL), RIMG_E_NOFILE);
    check_guards();
    fs_unmount("t");
}

TEST(nofile_and_io)
{
    loader_result_t r;

    setup();
    CHECK_EQ(load("t:/NONE.SYS", &r), RIMG_E_NOFILE);
    CHECK_EQ(load("t:/NODIR/NONE.SYS", &r), RIMG_E_NOFILE);
    CHECK_EQ(fs_mkdir("t:/DIR.SYS"), FS_OK);
    CHECK_EQ(load("t:/DIR.SYS", &r), RIMG_E_NOFILE);
    /* An unmounted volume is an I/O failure, as a failed mount is. */
    CHECK_EQ(load("q:/A.SYS", &r), RIMG_E_IO);
    /* Zero-length and short files cannot hold a header. */
    build(0x200);
    expect_header_err("t:/ZERO.SYS", 0, RIMG_E_IO);
    expect_header_err("t:/SHORT.SYS", 0x5F, RIMG_E_IO);
    fs_unmount("t");
    CHECK_EQ(load("t:/A.SYS", &r), RIMG_E_IO);
    check_slot_untouched();
}

TEST(header_errors_leave_slot_untouched)
{
    uint32_t size = 2u * LOADER_CHUNK;

    setup();
    build(size);
    memcpy(img + 0x20, RIMG_MAGIC_ROM1, 8);
    seal(size);
    expect_header_err("t:/MAGIC.SYS", size, RIMG_E_MAGIC);

    build(size);
    img[0x28] = 2;
    seal(size);
    expect_header_err("t:/VER.SYS", size, RIMG_E_VERSION);

    build(size);
    put32(img + 0x5C, 1); /* reserved */
    seal(size);
    expect_header_err("t:/RES.SYS", size, RIMG_E_VERSION);

    /* Header claims fewer bytes than the file holds. */
    build(size);
    put32(img + 0x34, size - 1);
    seal(size - 1);
    expect_header_err("t:/SMALL.SYS", size, RIMG_E_SIZE);

    /* Header claims more bytes than the file holds. */
    build(size);
    put32(img + 0x34, size + 1);
    expect_header_err("t:/LARGE.SYS", size, RIMG_E_SIZE);

    build(size);
    put32(img + 0x30, SLOT_BASE + 0x1000);
    seal(size);
    expect_header_err("t:/ADDR.SYS", size, RIMG_E_BOUNDS);

    build(size);
    put32(img + 0x38, SLOT_SIZE + 1);
    seal(size);
    expect_header_err("t:/MEM.SYS", size, RIMG_E_BOUNDS);

    build(size);
    put32(img + 0x38, size - 1);
    seal(size);
    expect_header_err("t:/MEMLT.SYS", size, RIMG_E_BOUNDS);

    build(size);
    put32(img + 0x48, 1); /* reserved BOOT subtype */
    seal(size);
    expect_header_err("t:/SUB.SYS", size, RIMG_E_BOUNDS);

    build(size);
    put32(img + 0x40, 1u << 16);
    seal(size);
    expect_header_err("t:/ABI.SYS", size, RIMG_E_ABI);
    fs_unmount("t");
}

TEST(file_larger_than_slot)
{
    uint32_t size = SLOT_SIZE + LOADER_CHUNK + 1;

    setup();
    build(size);
    expect_header_err("t:/BIG.SYS", size, RIMG_E_BOUNDS);
    /* Same file, header claiming only one slot's worth. */
    put32(img + 0x34, SLOT_SIZE);
    put32(img + 0x38, SLOT_SIZE);
    seal(SLOT_SIZE);
    expect_header_err("t:/BIG2.SYS", size, RIMG_E_SIZE);
    fs_unmount("t");
}

TEST(exactly_fills_slot)
{
    loader_result_t r;

    setup();
    build(SLOT_SIZE);
    write_file("t:/FULL.SYS", img, SLOT_SIZE);
    CHECK_EQ(load("t:/FULL.SYS", &r), RIMG_OK);
    CHECK_MEM(slot_mem, img, SLOT_SIZE);
    check_guards();
    fs_unmount("t");
}

TEST(crc_error_in_each_chunk)
{
    static const uint32_t at[] = { 0x61, LOADER_CHUNK + 5, 2u * LOADER_CHUNK, 3u * LOADER_CHUNK + 99 };
    uint32_t size = 3u * LOADER_CHUNK + 100;
    loader_result_t r;
    unsigned i;

    setup();
    for (i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        build(size);
        img[at[i]] ^= 0x40;
        write_file("t:/CRC.SYS", img, size);
        CHECK_EQ(load("t:/CRC.SYS", &r), RIMG_E_CRC);
        CHECK(r.hdr == NULL);
        /* The slot holds what was read, the bad byte included. */
        CHECK_MEM(slot_mem, img, size);
        check_guards();
    }
    fs_unmount("t");
}

TEST(chunk_boundaries)
{
    static const uint32_t k[] = { 1, 2, 5 };
    loader_result_t r;
    unsigned i;
    int d;

    setup();
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        for (d = -1; d <= 1; d++) {
            uint32_t size = k[i] * LOADER_CHUNK + (uint32_t)d;

            build(size);
            write_file("t:/EDGE.SYS", img, size);
            CHECK_EQ(load("t:/EDGE.SYS", &r), RIMG_OK);
            CHECK_EQ(r.file_size, size);
            CHECK_MEM(slot_mem, img, size);
            CHECK_EQ(slot_mem[size], FILL);
            check_guards();
        }
    }
    /* The smallest image: the header alone. */
    build(0x60);
    write_file("t:/MIN.SYS", img, 0x60);
    CHECK_EQ(load("t:/MIN.SYS", &r), RIMG_OK);
    CHECK_MEM(slot_mem, img, 0x60);
    check_guards();
    fs_unmount("t");
}

TEST(fallback)
{
    uint32_t size = 5000;
    loader_slot_t s = slot();
    loader_result_t r;

    setup();
    CHECK_EQ(fs_mkdir("t:/ROBCO"), FS_OK);
    build(size);
    write_file("t:/ROBCO/MFBOOT.BAK", img, size);

    /* Primary missing. */
    CHECK_EQ(loader_load_fallback("t:/ROBCO/MFBOOT.SYS", "t:/ROBCO/MFBOOT.BAK", &s,
                                  &boot_expect, &r), RIMG_OK);
    CHECK_EQ(r.fallback, 1);
    CHECK_EQ(r.primary_err, RIMG_E_NOFILE);
    CHECK_STR(r.path, "t:/ROBCO/MFBOOT.BAK");
    CHECK_MEM(slot_mem, img, size);

    /* Primary corrupt, backup good. */
    img[100] ^= 1;
    write_file("t:/ROBCO/MFBOOT.SYS", img, size);
    img[100] ^= 1;
    memset(arena, FILL, GUARD + SLOT_SIZE + GUARD);
    CHECK_EQ(loader_load_fallback("t:/ROBCO/MFBOOT.SYS", "t:/ROBCO/MFBOOT.BAK", &s,
                                  &boot_expect, &r), RIMG_OK);
    CHECK_EQ(r.fallback, 1);
    CHECK_EQ(r.primary_err, RIMG_E_CRC);
    CHECK_STR(r.path, "t:/ROBCO/MFBOOT.BAK");
    CHECK_MEM(slot_mem, img, size);
    check_guards();

    /* Primary good: no fallback. */
    write_file("t:/ROBCO/MFBOOT.SYS", img, size);
    CHECK_EQ(loader_load_fallback("t:/ROBCO/MFBOOT.SYS", "t:/ROBCO/MFBOOT.BAK", &s,
                                  &boot_expect, &r), RIMG_OK);
    CHECK_EQ(r.fallback, 0);
    CHECK_EQ(r.primary_err, RIMG_OK);
    CHECK_STR(r.path, "t:/ROBCO/MFBOOT.SYS");

    /* Both bad: the backup's code, the primary's reason. */
    put32(img + 0x40, 3u << 16);
    seal(size);
    write_file("t:/ROBCO/MFBOOT.BAK", img, size);
    CHECK_EQ(fs_unlink("t:/ROBCO/MFBOOT.SYS"), FS_OK);
    memset(arena, FILL, GUARD + SLOT_SIZE + GUARD);
    CHECK_EQ(loader_load_fallback("t:/ROBCO/MFBOOT.SYS", "t:/ROBCO/MFBOOT.BAK", &s,
                                  &boot_expect, &r), RIMG_E_ABI);
    CHECK_EQ(r.fallback, 1);
    CHECK_EQ(r.primary_err, RIMG_E_NOFILE);
    CHECK_EQ(r.err, RIMG_E_ABI);
    check_slot_untouched();

    /* Both missing. */
    CHECK_EQ(fs_unlink("t:/ROBCO/MFBOOT.BAK"), FS_OK);
    CHECK_EQ(loader_load_fallback("t:/ROBCO/MFBOOT.SYS", "t:/ROBCO/MFBOOT.BAK", &s,
                                  &boot_expect, &r), RIMG_E_NOFILE);
    CHECK_EQ(r.primary_err, RIMG_E_NOFILE);

    /* No backup given. */
    CHECK_EQ(loader_load_fallback("t:/ROBCO/MFBOOT.SYS", NULL, &s, &boot_expect, &r),
             RIMG_E_NOFILE);
    CHECK_EQ(r.fallback, 0);
    check_slot_untouched();
    fs_unmount("t");
}

/* A fake file for I/O faults: serves img and fails or stops early on request. */
typedef struct {
    uint32_t size;      /* size open() reports */
    uint32_t have;      /* bytes read() can serve */
    uint32_t pos;
    uint32_t fail_at;   /* read() fails once pos reaches this */
    int open_rc;
    int closed;
} fake_t;

static int fake_open(void *ctx, const char *path, uint64_t *size)
{
    fake_t *f = ctx;

    (void)path;
    f->pos = 0;
    *size = f->size;
    return f->open_rc;
}

static int fake_read(void *ctx, void *buf, uint32_t len)
{
    fake_t *f = ctx;

    if (f->pos >= f->fail_at) {
        return -1;
    }
    if (len > f->have - f->pos) {
        len = f->have - f->pos;
    }
    if (len > 777) {
        len = 777; /* short reads must be fine */
    }
    memcpy(buf, img + f->pos, len);
    f->pos += len;
    return (int)len;
}

static void fake_close(void *ctx)
{
    ((fake_t *)ctx)->closed++;
}

TEST(io_faults)
{
    uint32_t size = 3u * LOADER_CHUNK;
    fake_t f = { size, size, 0, 0xFFFFFFFFu, RIMG_OK, 0 };
    loader_io_t io = { fake_open, fake_read, fake_close, &f };
    loader_slot_t s = slot();
    loader_result_t r;

    build(size);
    memset(arena, FILL, GUARD + SLOT_SIZE + GUARD);
    CHECK_EQ(loader_load_io(&io, "x", &s, &boot_expect, &r), RIMG_OK);
    CHECK_EQ(f.closed, 1);
    CHECK_MEM(slot_mem, img, size);

    /* Open errors other than a missing file are I/O errors. */
    f.open_rc = FS_ERR_IO;
    CHECK_EQ(loader_load_io(&io, "x", &s, &boot_expect, &r), RIMG_E_IO);
    f.open_rc = RIMG_E_NOFILE;
    CHECK_EQ(loader_load_io(&io, "x", &s, &boot_expect, &r), RIMG_E_NOFILE);
    CHECK_EQ(f.closed, 1);
    f.open_rc = RIMG_OK;

    /* Read error in the header, then in the body. */
    memset(arena, FILL, GUARD + SLOT_SIZE + GUARD);
    f.fail_at = 0;
    CHECK_EQ(loader_load_io(&io, "x", &s, &boot_expect, &r), RIMG_E_IO);
    check_slot_untouched();
    f.fail_at = LOADER_CHUNK + 1;
    CHECK_EQ(loader_load_io(&io, "x", &s, &boot_expect, &r), RIMG_E_IO);
    check_guards();

    /* The file ends before the size the file system reported. */
    f.fail_at = 0xFFFFFFFFu;
    f.have = size - 10;
    CHECK_EQ(loader_load_io(&io, "x", &s, &boot_expect, &r), RIMG_E_IO);
    check_guards();
    CHECK_EQ(f.closed, 4);
}

TEST(bad_slot)
{
    fake_t f = { 0x100, 0x100, 0, 0xFFFFFFFFu, RIMG_OK, 0 };
    loader_io_t io = { fake_open, fake_read, fake_close, &f };
    loader_slot_t s = { 0xFFFF0000u, 0x20000u, slot_mem };
    loader_result_t r;

    CHECK_EQ(loader_load_io(&io, "x", &s, NULL, &r), RIMG_E_IO);
    s.size = 0;
    CHECK_EQ(loader_load_io(&io, "x", &s, NULL, &r), RIMG_E_IO);
    CHECK_EQ(loader_load_io(rc_launder(NULL), "x", &s, NULL, &r), RIMG_E_IO);
    CHECK_EQ(f.closed, 0);
}

TEST(long_path_is_cut)
{
    fake_t f = { 0, 0, 0, 0xFFFFFFFFu, RIMG_E_NOFILE, 0 };
    loader_io_t io = { fake_open, fake_read, fake_close, &f };
    loader_slot_t s = slot();
    loader_result_t r;
    char path[200];

    memset(path, 'a', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    CHECK_EQ(loader_load_io(&io, path, &s, NULL, &r), RIMG_E_NOFILE);
    CHECK_EQ(strlen(r.path), LOADER_PATH_MAX - 1);
}

TEST(check_header_matches_validate)
{
    rimg_expect_t e = { RIMG_MAGIC_BOOT, SLOT_BASE, SLOT_SIZE, 2, 0x1000 };

    build(0x1000);
    CHECK_EQ(rimg_check_header(img, 0x1000, &e), RIMG_OK);
    CHECK_EQ(rimg_validate(img, 0x1000, &e, NULL), RIMG_OK);
    CHECK_EQ(rimg_check_header(img, 0x5F, &e), RIMG_E_IO);
    CHECK_EQ(rimg_check_header(rc_launder(NULL), 0x1000, &e), RIMG_E_IO);
    img[0x200] ^= 1; /* body only: the header check does not see it */
    CHECK_EQ(rimg_check_header(img, 0x1000, &e), RIMG_OK);
    CHECK_EQ(rimg_validate(img, 0x1000, &e, NULL), RIMG_E_CRC);
    e.file_size = 0x1001;
    CHECK_EQ(rimg_check_header(img, 0x1001, &e), RIMG_E_SIZE);
}
