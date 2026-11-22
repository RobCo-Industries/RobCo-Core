#include <stddef.h>
#include <stdint.h>

#include <robco/bootinfo.h>
#include <robco/crc32.h>
#include <robco/klibc.h>

static uint32_t bi_crc(const robco_bootinfo_t *bi)
{
    static const uint8_t zero[4];
    const uint8_t *p = (const uint8_t *)bi;
    uint32_t crc;

    crc = crc32_update(0, p, offsetof(robco_bootinfo_t, crc32));
    crc = crc32_update(crc, zero, sizeof(zero));
    return crc32_update(crc, p + offsetof(robco_bootinfo_t, crc32) + 4,
                        ROBCO_BOOTINFO_SIZE - offsetof(robco_bootinfo_t, crc32) - 4);
}

void bootinfo_seal(robco_bootinfo_t *bi)
{
    bi->magic = ROBCO_BOOTINFO_MAGIC;
    bi->version = ROBCO_BOOTINFO_VERSION;
    bi->size = ROBCO_BOOTINFO_SIZE;
    bi->crc32 = bi_crc(bi);
}

int bootinfo_validate(const robco_bootinfo_t *bi)
{
    if (bi == NULL || bi->magic != ROBCO_BOOTINFO_MAGIC) {
        return BOOTINFO_E_MAGIC;
    }
    if (bi->version != ROBCO_BOOTINFO_VERSION) {
        return BOOTINFO_E_VERSION;
    }
    if (bi->size != ROBCO_BOOTINFO_SIZE) {
        return BOOTINFO_E_SIZE;
    }
    if (bi->crc32 != bi_crc(bi)) {
        return BOOTINFO_E_CRC;
    }
    return BOOTINFO_OK;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

uint32_t bootinfo_fw_blob_size(const void *blob, uint32_t max)
{
    const uint8_t *p = blob;
    uint32_t off = 0;
    uint32_t size;

    if (p == NULL || max < 8) {
        return 0;
    }
    if (be32(p) == ROBCO_FDT_MAGIC) {
        size = be32(p + 4);
        /* 40 bytes is the smallest FDT header. */
        return (size >= 40 && size <= max) ? size : 0;
    }
    if (le32(p + 4) != ROBCO_ATAG_CORE) {
        return 0;
    }
    for (;;) {
        if (max - off < 8) {
            return 0;
        }
        size = le32(p + off);
        if (le32(p + off + 4) == ROBCO_ATAG_NONE) {
            return off + 8;
        }
        if (size < 2 || size > (max - off) / 4) {
            return 0;
        }
        off += size * 4;
    }
}

#define CAPS_UART   ROBCO_CAP_PL011
#define CAPS_BOARD  ROBCO_CAP_LOCAL_INTC

void bootinfo_merge(robco_bootinfo_t *out, const robco_bootinfo_t *prev,
                    const robco_bootinfo_t *own, uint32_t own_groups)
{
    robco_bootinfo_t m;

    if (prev == NULL) {
        k_memmove(out, own, sizeof(*out));
        return;
    }
    k_memcpy(&m, prev, sizeof(m));

    m.stage = own->stage;
    m.board_rev = own->board_rev;
    m.soc = own->soc;
    m.periph_base = own->periph_base;
    m.midr = own->midr;
    m.mem_base = own->mem_base;
    m.mem_size = own->mem_size;
    m.caps = (m.caps & ~CAPS_BOARD) | (own->caps & CAPS_BOARD);

    if (own_groups & BOOTINFO_OWN_FW_REGS) {
        m.fw_r0 = own->fw_r0;
        m.fw_r1 = own->fw_r1;
        m.fw_r2 = own->fw_r2;
    }
    if (own_groups & BOOTINFO_OWN_FW_BLOB) {
        m.fdt_addr = own->fdt_addr;
        m.fdt_size = own->fdt_size;
    }
    if (own_groups & BOOTINFO_OWN_UART) {
        m.uart_type = own->uart_type;
        m.uart_clock = own->uart_clock;
        m.uart_baud = own->uart_baud;
        m.caps = (m.caps & ~CAPS_UART) | (own->caps & CAPS_UART);
    }
    if (own_groups & BOOTINFO_OWN_MINIUART) {
        m.caps = (m.caps & ~ROBCO_CAP_MINIUART) | (own->caps & ROBCO_CAP_MINIUART);
    }
    if (own_groups & BOOTINFO_OWN_SD) {
        m.caps = (m.caps & ~ROBCO_CAP_SD) | (own->caps & ROBCO_CAP_SD);
    }
    if (own_groups & BOOTINFO_OWN_FB) {
        m.fb_addr = own->fb_addr;
        m.fb_size = own->fb_size;
        m.fb_width = own->fb_width;
        m.fb_height = own->fb_height;
        m.fb_pitch = own->fb_pitch;
        m.fb_depth = own->fb_depth;
        m.fb_order = own->fb_order;
        m.caps = (m.caps & ~ROBCO_CAP_FB) | (own->caps & ROBCO_CAP_FB);
        if (!(own_groups & BOOTINFO_OWN_CON)) {
            /* The old console drew on the old framebuffer. Keep only the scheme. */
            m.con_cols = m.con_rows = m.con_cx = m.con_cy = 0;
        }
    }
    if (own_groups & BOOTINFO_OWN_CON) {
        m.con_cols = own->con_cols;
        m.con_rows = own->con_rows;
        m.con_cx = own->con_cx;
        m.con_cy = own->con_cy;
        m.con_scheme = own->con_scheme;
    }
    if (own_groups & BOOTINFO_OWN_BOOT_DEV) {
        m.boot_dev = own->boot_dev;
        m.boot_part_lba = own->boot_part_lba;
        m.boot_flags = own->boot_flags;
        k_memcpy(m.boot_path, own->boot_path, sizeof(m.boot_path));
        k_memcpy(m.cmdline, own->cmdline, sizeof(m.cmdline));
    }
    k_memcpy(out, &m, sizeof(m));
}
