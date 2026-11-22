#include <stddef.h>
#include <stdint.h>

#include <robco/bootinfo.h>
#include <robco/crc32.h>

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
