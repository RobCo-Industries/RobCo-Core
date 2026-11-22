#include <robco/crc32.h>
#include <robco/rimg.h>

#include "rc_test.h"

#define IMG_SIZE 0x200

static uint8_t img[IMG_SIZE + 0x100];

static rimg_header_t *hdr(void)
{
    return (rimg_header_t *)(img + RIMG_HDR_OFFSET);
}

static void reseal(void)
{
    uint32_t file_size = hdr()->file_size;

    hdr()->crc32 = 0;
    hdr()->crc32 = crc32(img, file_size <= sizeof(img) ? file_size : sizeof(img));
}

/* A sealed BOOT example image at 0x8000, IMG_SIZE bytes. */
static void build(void)
{
    rimg_header_t *h = hdr();
    size_t i;

    memset(img, 0, sizeof(img));
    img[0] = 0x16;
    img[3] = 0xea; /* b 0x60 */
    memcpy(h->magic, RIMG_MAGIC_BOOT, 8);
    h->hdr_version = RIMG_HDR_VERSION;
    h->hdr_size = RIMG_HDR_SIZE;
    h->load_addr = 0x8000;
    h->file_size = IMG_SIZE;
    h->mem_size = IMG_SIZE + 0x4000;
    h->abi_version = (2 << 16) | 3;
    h->subtype = RIMG_BOOT_EXAMPLE;
    memcpy(h->name, "test", 4);
    for (i = 0x60; i < IMG_SIZE; i++) {
        img[i] = (uint8_t)i;
    }
    reseal();
}

static int check(const rimg_expect_t *e)
{
    return rimg_validate(img, IMG_SIZE, e, NULL);
}

TEST(ok_default_slot)
{
    const rimg_header_t *out = NULL;

    build();
    CHECK_EQ(rimg_validate(img, IMG_SIZE, NULL, &out), RIMG_OK);
    CHECK(out == hdr());
}

TEST(ok_explicit_expect_and_crc_matches_zlib_rule)
{
    rimg_expect_t e = { RIMG_MAGIC_BOOT, 0x200000, 0x200000, 2, IMG_SIZE };

    build();
    hdr()->load_addr = 0x200000;
    hdr()->subtype = RIMG_BOOT_AGENT;
    reseal();
    CHECK_EQ(check(&e), RIMG_OK);
    /* Extra readable bytes after the file do not matter. */
    CHECK_EQ(rimg_validate(img, sizeof(img), &e, NULL), RIMG_OK);
}

TEST(io_short)
{
    build();
    CHECK_EQ(rimg_validate(img, 0x5f, NULL, NULL), RIMG_E_IO);
    CHECK_EQ(rimg_validate(NULL, IMG_SIZE, NULL, NULL), RIMG_E_IO);
}

TEST(magic)
{
    rimg_expect_t e = { RIMG_MAGIC_ROM1, 0, 0, 0, 0 };

    build();
    CHECK_EQ(check(&e), RIMG_E_MAGIC);
    memcpy(hdr()->magic, "BOGUS\0\0\0", 8);
    reseal();
    CHECK_EQ(check(NULL), RIMG_E_MAGIC);
    memset(img, 0, sizeof(img));
    CHECK_EQ(check(NULL), RIMG_E_MAGIC);
}

TEST(version)
{
    build();
    hdr()->hdr_version = 2;
    CHECK_EQ(check(NULL), RIMG_E_VERSION);
    build();
    hdr()->hdr_size = 60;
    CHECK_EQ(check(NULL), RIMG_E_VERSION);
    build();
    hdr()->flags = 4;
    CHECK_EQ(check(NULL), RIMG_E_VERSION);
    build();
    hdr()->reserved = 1;
    CHECK_EQ(check(NULL), RIMG_E_VERSION);
}

TEST(size)
{
    rimg_expect_t e = { NULL, 0, 0, 0, IMG_SIZE + 4 };

    build();
    CHECK_EQ(check(&e), RIMG_E_SIZE);
    CHECK_EQ(rimg_validate(img, IMG_SIZE - 1, NULL, NULL), RIMG_E_SIZE);
    hdr()->file_size = 0x5c;
    CHECK_EQ(check(NULL), RIMG_E_SIZE);
    build();
    hdr()->flags = RIMG_FLAG_META;
    hdr()->file_size = 0x100;
    CHECK_EQ(check(NULL), RIMG_E_SIZE);
}

TEST(bounds)
{
    rimg_expect_t e = { NULL, 0x200000, 0x200000, 0, 0 };

    build();
    CHECK_EQ(check(&e), RIMG_E_BOUNDS);
    hdr()->subtype = 1;
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
    build();
    hdr()->load_addr = 0x9000;
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
    build();
    hdr()->mem_size = IMG_SIZE - 1;
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
    build();
    hdr()->mem_size = 0xf8001;
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
    build();
    hdr()->mem_size = 0xfffffff0u; /* no 32-bit wrap */
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
    build();
    memcpy(hdr()->magic, RIMG_MAGIC_ROBCO78, 8);
    hdr()->subtype = RIMG_HOLO_DATA;
    hdr()->load_addr = 0x800000;
    hdr()->abi_version = 1 << 16;
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
}

TEST(abi)
{
    rimg_expect_t e = { NULL, 0, 0, 3, 0 };

    build();
    CHECK_EQ(check(&e), RIMG_E_ABI);
    hdr()->abi_version = 1 << 16;
    CHECK_EQ(check(NULL), RIMG_E_ABI);
}

TEST(crc)
{
    build();
    img[0x100] ^= 0x01;
    CHECK_EQ(check(NULL), RIMG_E_CRC);
    build();
    hdr()->crc32 ^= 1;
    CHECK_EQ(check(NULL), RIMG_E_CRC);
}

TEST(order)
{
    build();
    hdr()->hdr_version = 9;
    img[0x100] ^= 1;
    memcpy(hdr()->magic, "XXXX\0\0\0\0", 8);
    CHECK_EQ(check(NULL), RIMG_E_MAGIC);
    build();
    hdr()->load_addr = 0x9000;
    hdr()->abi_version = 5 << 16;
    CHECK_EQ(check(NULL), RIMG_E_BOUNDS);
}

TEST(strerror)
{
    CHECK_STR(rimg_strerror(RIMG_OK), "ok");
    CHECK_STR(rimg_strerror(RIMG_E_NOFILE), "nofile");
    CHECK_STR(rimg_strerror(RIMG_E_MAGIC), "magic");
    CHECK_STR(rimg_strerror(RIMG_E_CRC), "crc");
    CHECK_STR(rimg_strerror(9), "unknown");
    CHECK_STR(rimg_strerror(-1), "unknown");
}
