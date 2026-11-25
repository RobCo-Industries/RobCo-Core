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

static void fill_prev(robco_bootinfo_t *p)
{
    memset(p, 0, sizeof(*p));
    p->stage = ROBCO_STAGE_BIOS;
    p->mem_size = 0x1000;
    p->fw_r1 = 0xc43;
    p->fdt_addr = ROBCO_HANDOFF_FDT_ADDR;
    p->fdt_size = 0x200;
    p->caps = ROBCO_CAP_FB | ROBCO_CAP_PL011 | ROBCO_CAP_SD;
    p->uart_type = ROBCO_UART_PL011;
    p->uart_baud = 115200;
    p->fb_addr = 0x3c100000;
    p->fb_width = 640;
    p->fb_height = 480;
    p->con_cols = 80;
    p->con_rows = 30;
    p->con_cy = 5;
    p->con_scheme = 2;
    p->boot_dev = ROBCO_BOOT_DEV_EMMC;
    p->boot_flags = ROBCO_BOOT_FALLBACK;
    memcpy(p->boot_path, "/A", 3);
}

static void fill_own(robco_bootinfo_t *o)
{
    memset(o, 0, sizeof(*o));
    o->stage = ROBCO_STAGE_BOOTAGENT;
    o->mem_size = 0x2000;
    o->caps = ROBCO_CAP_LOCAL_INTC;
    o->fw_r1 = 0x99;
    o->boot_flags = ROBCO_BOOT_COLD;
}

TEST(merge_keeps_inherited_fb_and_console)
{
    robco_bootinfo_t prev, own, out;

    fill_prev(&prev);
    fill_own(&own);
    bootinfo_merge(&out, &prev, &own, 0);
    CHECK_EQ(out.stage, ROBCO_STAGE_BOOTAGENT);
    CHECK_EQ(out.mem_size, 0x2000);
    CHECK_EQ(out.caps, ROBCO_CAP_FB | ROBCO_CAP_PL011 | ROBCO_CAP_SD | ROBCO_CAP_LOCAL_INTC);
    CHECK_EQ(out.fb_addr, 0x3c100000);
    CHECK_EQ(out.fb_width, 640);
    CHECK_EQ(out.con_cols, 80);
    CHECK_EQ(out.con_cy, 5);
    CHECK_EQ(out.con_scheme, 2);
    CHECK_EQ(out.uart_baud, 115200);
    CHECK_EQ(out.fw_r1, 0xc43);
    CHECK_EQ(out.fdt_size, 0x200);
    CHECK_EQ(out.boot_dev, ROBCO_BOOT_DEV_EMMC);
    CHECK_EQ(out.boot_flags, ROBCO_BOOT_FALLBACK);
    CHECK_MEM(out.boot_path, "/A", 3);
}

TEST(boot_flags_bits)
{
    robco_bootinfo_t prev, own, out;

    CHECK_EQ(ROBCO_BOOT_FALLBACK, 0x1u);
    CHECK_EQ(ROBCO_BOOT_MAINT, 0x2u);
    CHECK_EQ(ROBCO_BOOT_COLD, 0x4u);
    CHECK_EQ(ROBCO_BOOT_MAINT_REQ, 0x8u);

    fill_prev(&prev);
    fill_own(&own);
    prev.boot_flags = ROBCO_BOOT_MAINT | ROBCO_BOOT_MAINT_REQ;
    bootinfo_merge(&out, &prev, &own, 0);
    CHECK_EQ(out.boot_flags, ROBCO_BOOT_MAINT | ROBCO_BOOT_MAINT_REQ);
}

TEST(merge_own_groups_replace)
{
    robco_bootinfo_t prev, own, out;

    fill_prev(&prev);
    fill_own(&own);
    own.fdt_addr = 0x5000;
    own.fdt_size = 0x40;
    own.uart_type = ROBCO_UART_MINI;
    own.uart_baud = 9600;
    own.caps |= ROBCO_CAP_MINIUART;
    own.fb_addr = 0x3d000000;
    own.fb_width = 1024;
    own.caps |= ROBCO_CAP_FB;
    own.con_cols = 128;
    own.con_scheme = 1;
    bootinfo_merge(&out, &prev, &own,
                   BOOTINFO_OWN_FW_BLOB | BOOTINFO_OWN_UART | BOOTINFO_OWN_MINIUART |
                   BOOTINFO_OWN_FB | BOOTINFO_OWN_CON);
    CHECK_EQ(out.fdt_addr, 0x5000);
    CHECK_EQ(out.fdt_size, 0x40);
    CHECK_EQ(out.uart_type, ROBCO_UART_MINI);
    CHECK_EQ(out.uart_baud, 9600);
    CHECK_EQ(out.caps, ROBCO_CAP_MINIUART | ROBCO_CAP_FB | ROBCO_CAP_SD | ROBCO_CAP_LOCAL_INTC);
    CHECK_EQ(out.fb_addr, 0x3d000000);
    CHECK_EQ(out.fb_width, 1024);
    CHECK_EQ(out.con_cols, 128);
    CHECK_EQ(out.con_cy, 0);
    CHECK_EQ(out.con_scheme, 1);
    CHECK_EQ(out.fw_r1, 0xc43);
}

TEST(merge_sd_and_mini_uart_bits)
{
    robco_bootinfo_t prev, own, out;

    /* Not set up here: both bits carry over. */
    fill_prev(&prev);
    prev.caps |= ROBCO_CAP_MINIUART;
    fill_own(&own);
    bootinfo_merge(&out, &prev, &own, 0);
    CHECK_EQ(out.caps & (ROBCO_CAP_SD | ROBCO_CAP_MINIUART), ROBCO_CAP_SD | ROBCO_CAP_MINIUART);

    /* Set up here: this stage's bits win, and the console UART group leaves them alone. */
    fill_prev(&prev);
    prev.caps &= ~ROBCO_CAP_SD;
    fill_own(&own);
    own.caps |= ROBCO_CAP_SD | ROBCO_CAP_MINIUART;
    bootinfo_merge(&out, &prev, &own, BOOTINFO_OWN_SD | BOOTINFO_OWN_MINIUART | BOOTINFO_OWN_UART);
    CHECK_EQ(out.caps & (ROBCO_CAP_SD | ROBCO_CAP_MINIUART), ROBCO_CAP_SD | ROBCO_CAP_MINIUART);
    CHECK_EQ(out.caps & ROBCO_CAP_PL011, 0);

    /* A group without its bit clears it. */
    fill_prev(&prev);
    prev.caps |= ROBCO_CAP_MINIUART;
    fill_own(&own);
    bootinfo_merge(&out, &prev, &own, BOOTINFO_OWN_SD | BOOTINFO_OWN_MINIUART);
    CHECK_EQ(out.caps & (ROBCO_CAP_SD | ROBCO_CAP_MINIUART), 0);
    CHECK_EQ(out.caps & ROBCO_CAP_PL011, ROBCO_CAP_PL011);
}

TEST(merge_new_fb_drops_old_console)
{
    robco_bootinfo_t prev, own, out;

    fill_prev(&prev);
    fill_own(&own);
    own.fb_addr = 0x3d000000;
    own.caps |= ROBCO_CAP_FB;
    bootinfo_merge(&out, &prev, &own, BOOTINFO_OWN_FB);
    CHECK_EQ(out.fb_addr, 0x3d000000);
    CHECK_EQ(out.con_cols, 0);
    CHECK_EQ(out.con_rows, 0);
    CHECK_EQ(out.con_cy, 0);
    CHECK_EQ(out.con_scheme, 2);
}

TEST(merge_cold_and_alias)
{
    robco_bootinfo_t prev, own;

    fill_own(&own);
    fill_prev(&prev);
    bootinfo_merge(&prev, NULL, &own, 0);
    CHECK_MEM(&prev, &own, sizeof(own));

    fill_prev(&prev);
    bootinfo_merge(&prev, &prev, &own, 0);
    CHECK_EQ(prev.fb_addr, 0x3c100000);
    CHECK_EQ(prev.stage, ROBCO_STAGE_BOOTAGENT);
}
