#include <robco/klibc.h>
#include <robco/part.h>

#define MBR_TABLE 446u
#define MBR_ENTRY 16u

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Same checks FatFs makes: a jump, 512-byte sectors, a sane BPB. */
static int is_fat_vbr(const uint8_t *b)
{
    unsigned spc = b[13];

    if (!(b[0] == 0xE9 || (b[0] == 0xEB && b[2] == 0x90))) {
        return 0;
    }
    return le16(b + 11) == 512 && spc != 0 && (spc & (spc - 1)) == 0 &&
           le16(b + 14) != 0 && (b[16] == 1 || b[16] == 2);
}

int part_scan(blk_dev_t *dev, part_info_t *out, unsigned max)
{
    uint8_t b[BLK_SECTOR_SIZE];
    unsigned i, n = 0;
    int rc;

    if (dev == NULL || (out == NULL && max != 0) || dev->block_size != BLK_SECTOR_SIZE) {
        return BLK_ERR_PARAM;
    }
    rc = blk_read(dev, 0, 1, b);
    if (rc != 0) {
        return rc;
    }
    if (b[510] != 0x55 || b[511] != 0xAA) {
        return 0;
    }
    if (is_fat_vbr(b)) {
        if (max == 0) {
            return 0;
        }
        k_memset(out, 0, sizeof(*out));
        out->dev = dev;
        out->superfloppy = 1;
        out->block_count = dev->block_count;
        return 1;
    }
    for (i = 0; i < PART_MAX; i++) {
        uint8_t status = b[MBR_TABLE + i * MBR_ENTRY];

        if (status != 0x00 && status != 0x80) {
            return 0; /* not a partition table */
        }
    }
    for (i = 0; i < PART_MAX && n < max; i++) {
        const uint8_t *e = b + MBR_TABLE + i * MBR_ENTRY;
        uint64_t start = le32(e + 8);
        uint64_t count = le32(e + 12);

        if (e[4] == 0 || start == 0 || count == 0) {
            continue;
        }
        if (dev->block_count != 0 && (start >= dev->block_count || count > dev->block_count - start)) {
            continue;
        }
        k_memset(&out[n], 0, sizeof(out[n]));
        out[n].dev = dev;
        out[n].index = (uint8_t)i;
        out[n].type = e[4];
        out[n].bootable = e[0] == 0x80;
        out[n].start_lba = start;
        out[n].block_count = count;
        n++;
    }
    return (int)n;
}

static blk_dev_t *part_parent(blk_dev_t *dev, const part_info_t **p)
{
    *p = dev->priv;
    return (*p)->dev;
}

static int part_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    const part_info_t *p;
    blk_dev_t *parent = part_parent(dev, &p);

    return blk_read(parent, p->start_lba + lba, count, buf);
}

static int part_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    const part_info_t *p;
    blk_dev_t *parent = part_parent(dev, &p);

    return blk_write(parent, p->start_lba + lba, count, buf);
}

static int part_sync(blk_dev_t *dev)
{
    const part_info_t *p;

    return blk_sync(part_parent(dev, &p));
}

int part_open(blk_dev_t *dev, const part_info_t *p, blk_dev_t *out)
{
    if (dev == NULL || p == NULL || out == NULL || p->dev != dev || p->block_count == 0) {
        return BLK_ERR_PARAM;
    }
    if (dev->block_count != 0 &&
        (p->start_lba >= dev->block_count || p->block_count > dev->block_count - p->start_lba)) {
        return BLK_ERR_RANGE;
    }
    out->name = dev->name;
    out->block_size = dev->block_size;
    out->block_count = p->block_count;
    out->flags = dev->flags;
    out->read = part_read;
    out->write = part_write;
    out->sync = part_sync;
    out->priv = (void *)(uintptr_t)p;
    return 0;
}

blk_dev_t *part_parent_of(const blk_dev_t *dev, uint64_t *start_lba)
{
    const part_info_t *p;

    if (dev == NULL || dev->read != part_read || dev->priv == NULL) {
        return NULL;
    }
    p = dev->priv;
    if (start_lba != NULL) {
        *start_lba = p->start_lba;
    }
    return p->dev;
}
