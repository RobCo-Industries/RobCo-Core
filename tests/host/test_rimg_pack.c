/*
 * The C side of the rimg pack round trip. mk/host.mk packs these images
 * with tools/rimg.py pack and checks them with tools/rimg.py verify;
 * rimg_validate() must accept the same bytes and read the same META.
 */
#include <robco/rimg.h>

#include "rc_test.h"

#ifndef ROBCO_HOST_RIMG_DIR
#define ROBCO_HOST_RIMG_DIR "build/host/rimg"
#endif

#define PAYLOAD "payload bytes for the rimg pack round trip\n"

static uint8_t buf[4096];

static size_t load(const char *name)
{
    char path[256];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "%s/%s", ROBCO_HOST_RIMG_DIR, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return 0;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n;
}

static const rimg_meta_t *meta(void)
{
    return (const rimg_meta_t *)(buf + RIMG_META_OFFSET);
}

/* Bit x of row y of the frame-and-diagonal icon in tests/host/icon.pbm. */
static int icon_bit(unsigned x, unsigned y)
{
    return x == 0 || x == 31 || y == 0 || y == 31 || x == y;
}

TEST(rom1_with_title_and_icon)
{
    rimg_expect_t e = { RIMG_MAGIC_ROM1, 0, 0, 0, 0 };
    const rimg_header_t *h = NULL;
    size_t n = load("rom1.img");
    unsigned x, y;
    int bad = 0;

    CHECK_EQ(n, RIMG_META_OFFSET + RIMG_META_SIZE + sizeof(PAYLOAD) - 1);
    e.file_size = (uint32_t)n;
    CHECK_EQ(rimg_validate(buf, n, &e, &h), RIMG_OK);
    if (h == NULL) {
        return;
    }
    CHECK_EQ(h->flags, RIMG_FLAG_META);
    CHECK_EQ(h->load_addr, 0x400000);
    CHECK_EQ(h->subtype, RIMG_ROM1_SYSTEM);
    CHECK_EQ(h->abi_version, 1u << 16);
    CHECK_EQ(h->image_version, (3u << 16) | (0 << 8) | 3);
    CHECK_STR(h->name, "DEITRIX");
    CHECK_STR(meta()->title, "Deitrix test ROM");
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            int lit = (meta()->icon[y * 4 + x / 8] >> (7 - x % 8)) & 1;

            bad += lit != icon_bit(x, y);
        }
    }
    CHECK_EQ(bad, 0);
    /* Word 0 branches to the payload after META: b 0x120. */
    CHECK_EQ(buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24, 0xEA000046u);
    CHECK_MEM(buf + RIMG_META_OFFSET + RIMG_META_SIZE, PAYLOAD, sizeof(PAYLOAD) - 1);
}

TEST(holotape_blank_meta)
{
    const rimg_header_t *h = NULL;
    size_t n = load("holo.img");
    size_t i;
    int lit = 0;

    CHECK_EQ(rimg_validate(buf, n, NULL, &h), RIMG_OK);
    if (h == NULL) {
        return;
    }
    CHECK_EQ(h->mem_size, 0x10000);
    CHECK_EQ(h->subtype, RIMG_HOLO_UTILITY);
    CHECK_EQ(meta()->title[0], 0);
    for (i = 0; i < sizeof(meta()->icon); i++) {
        lit |= meta()->icon[i];
    }
    CHECK_EQ(lit, 0);
}

TEST(boot_without_meta)
{
    const rimg_header_t *h = NULL;
    size_t n = load("boot.img");

    CHECK_EQ(rimg_validate(buf, n, NULL, &h), RIMG_OK);
    if (h == NULL) {
        return;
    }
    CHECK_EQ(h->flags, 0);
    CHECK_EQ(h->file_size, RIMG_META_OFFSET + sizeof(PAYLOAD) - 1);
    CHECK_MEM(buf + RIMG_META_OFFSET, PAYLOAD, sizeof(PAYLOAD) - 1);
}

TEST(c_rejects_what_python_rejects)
{
    size_t n = load("rom1.img");

    /* A flipped payload byte fails the CRC on both sides. */
    buf[n - 1] ^= 0xFF;
    CHECK_EQ(rimg_validate(buf, n, NULL, NULL), RIMG_E_CRC);
    buf[n - 1] ^= 0xFF;
    /* Clearing META on a ROM fails step 2. */
    ((rimg_header_t *)(buf + RIMG_HDR_OFFSET))->flags = 0;
    CHECK_EQ(rimg_validate(buf, n, NULL, NULL), RIMG_E_VERSION);
}
