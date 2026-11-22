#include <stddef.h>

#include <robco/bootinfo.h>
#include <robco/crc32.h>

#include "rc_test.h"

static robco_bootinfo_t bi;

static void build(void)
{
    memset(&bi, 0, sizeof(bi));
    bi.stage = ROBCO_STAGE_BIOS;
    bi.soc = 0x2836;
    bi.midr = 0x410fc075;
    bi.mem_size = 0x3c000000;
    bi.fw_r1 = 0xc43;
    bi.fw_r2 = 0x100;
    memcpy(bi.boot_path, "/ROBCO/MFBOOT.SYS", 18);
    bootinfo_seal(&bi);
}

TEST(seal_fills_header)
{
    robco_bootinfo_t copy;

    build();
    CHECK_EQ(bi.magic, ROBCO_BOOTINFO_MAGIC);
    CHECK_EQ(bi.version, 2);
    CHECK_EQ(bi.size, 320);
    CHECK_MEM(&bi, "CBOR", 4);
    copy = bi;
    copy.crc32 = 0;
    CHECK_EQ(bi.crc32, crc32(&copy, sizeof(copy)));
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_OK);
}

TEST(validate_null)
{
    CHECK_EQ(bootinfo_validate(NULL), BOOTINFO_E_MAGIC);
}

TEST(validate_magic)
{
    build();
    bi.magic ^= 1;
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_MAGIC);
    memset(&bi, 0, sizeof(bi));
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_MAGIC);
}

TEST(validate_version)
{
    build();
    bi.version = 1;
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_VERSION);
}

TEST(validate_size)
{
    build();
    bi.size = 256;
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_SIZE);
}

TEST(validate_crc)
{
    build();
    bi.crc32 ^= 0x80000000u;
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_CRC);
    build();
    bi.fw_r1 ^= 1;
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_CRC);
    build();
    bi.cmdline[127] = 'x';
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_E_CRC);
    bootinfo_seal(&bi);
    CHECK_EQ(bootinfo_validate(&bi), BOOTINFO_OK);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The list the firmware stub writes: ATAG_CORE, ATAG_MEM, ATAG_NONE. */
static size_t atags(uint8_t *p)
{
    put32(p + 0, 5);
    put32(p + 4, ROBCO_ATAG_CORE);
    put32(p + 20, 4);
    put32(p + 24, 0x54410002);
    put32(p + 28, 0x20000000);
    put32(p + 36, 0);
    put32(p + 40, ROBCO_ATAG_NONE);
    return 44;
}

TEST(blob_atags)
{
    uint8_t buf[128] = { 0 };
    size_t n = atags(buf);

    CHECK_EQ(bootinfo_fw_blob_size(buf, sizeof(buf)), n);
    CHECK_EQ(bootinfo_fw_blob_size(buf, (uint32_t)n), n);
    CHECK_EQ(bootinfo_fw_blob_size(buf, (uint32_t)n - 1), 0);
}

TEST(blob_atags_unterminated)
{
    uint8_t buf[64] = { 0 };

    put32(buf, 2);
    put32(buf + 4, ROBCO_ATAG_CORE);
    put32(buf + 8, 2);
    put32(buf + 12, 0x54410002);
    put32(buf + 16, 1); /* size below 2 */
    put32(buf + 20, 0x54410003);
    CHECK_EQ(bootinfo_fw_blob_size(buf, sizeof(buf)), 0);
    put32(buf + 16, 200); /* runs past max */
    CHECK_EQ(bootinfo_fw_blob_size(buf, sizeof(buf)), 0);
}

TEST(blob_fdt)
{
    uint8_t buf[256] = { 0 };

    put_be32(buf, ROBCO_FDT_MAGIC);
    put_be32(buf + 4, 200);
    CHECK_EQ(bootinfo_fw_blob_size(buf, sizeof(buf)), 200);
    CHECK_EQ(bootinfo_fw_blob_size(buf, 199), 0);
    put_be32(buf + 4, 8);
    CHECK_EQ(bootinfo_fw_blob_size(buf, sizeof(buf)), 0);
}

TEST(blob_none)
{
    uint8_t buf[64] = { 0 };

    CHECK_EQ(bootinfo_fw_blob_size(buf, sizeof(buf)), 0);
    CHECK_EQ(bootinfo_fw_blob_size(NULL, sizeof(buf)), 0);
    CHECK_EQ(bootinfo_fw_blob_size(buf, 4), 0);
}
