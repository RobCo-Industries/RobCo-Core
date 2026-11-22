#include <robco/blk_ram.h>
#include <robco/klibc.h>

static int ram_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    const unsigned char *base = dev->priv;

    k_memcpy(buf, base + (size_t)lba * BLK_SECTOR_SIZE, (size_t)count * BLK_SECTOR_SIZE);
    return 0;
}

static int ram_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    unsigned char *base = dev->priv;

    k_memcpy(base + (size_t)lba * BLK_SECTOR_SIZE, buf, (size_t)count * BLK_SECTOR_SIZE);
    return 0;
}

int blk_ram_init(blk_dev_t *dev, void *buf, size_t size, uint32_t flags)
{
    if (dev == NULL || buf == NULL || size < BLK_SECTOR_SIZE) {
        return BLK_ERR_PARAM;
    }
    dev->name = "ram";
    dev->block_size = BLK_SECTOR_SIZE;
    dev->block_count = size / BLK_SECTOR_SIZE;
    dev->flags = flags;
    dev->read = ram_read;
    dev->write = ram_write;
    dev->sync = NULL;
    dev->priv = buf;
    return 0;
}
