#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blk_file.h"

static int file_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    FILE *fp = dev->priv;

    if (fseeko(fp, (off_t)(lba * BLK_SECTOR_SIZE), SEEK_SET) != 0 ||
        fread(buf, BLK_SECTOR_SIZE, count, fp) != count) {
        return BLK_ERR_IO;
    }
    return 0;
}

static int file_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    FILE *fp = dev->priv;

    if (fseeko(fp, (off_t)(lba * BLK_SECTOR_SIZE), SEEK_SET) != 0 ||
        fwrite(buf, BLK_SECTOR_SIZE, count, fp) != count) {
        return BLK_ERR_IO;
    }
    return 0;
}

static int file_sync(blk_dev_t *dev)
{
    return fflush((FILE *)dev->priv) == 0 ? 0 : BLK_ERR_IO;
}

int blk_file_open(blk_dev_t *dev, const char *path, uint32_t flags)
{
    int ro = (flags & BLK_FLAG_READONLY) != 0;
    FILE *fp = fopen(path, ro ? "rb" : "r+b");
    off_t size;

    if (fp == NULL) {
        return -1;
    }
    if (fseeko(fp, 0, SEEK_END) != 0 || (size = ftello(fp)) < BLK_SECTOR_SIZE) {
        fclose(fp);
        return -1;
    }
    memset(dev, 0, sizeof(*dev));
    dev->name = "file";
    dev->block_size = BLK_SECTOR_SIZE;
    dev->block_count = (uint64_t)size / BLK_SECTOR_SIZE;
    dev->flags = flags;
    dev->read = file_read;
    dev->write = file_write;
    dev->sync = file_sync;
    dev->priv = fp;
    return 0;
}

void blk_file_close(blk_dev_t *dev)
{
    if (dev->priv != NULL) {
        fclose(dev->priv);
        dev->priv = NULL;
    }
}

static int fault_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    blk_fault_t *f = dev->priv;

    f->reads++;
    if (f->fail_read_after >= 0 && f->reads > (unsigned long)f->fail_read_after) {
        return f->err;
    }
    return blk_read(f->inner, lba, count, buf);
}

static int fault_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    blk_fault_t *f = dev->priv;

    f->writes++;
    if (f->fail_write_after >= 0 && f->writes > (unsigned long)f->fail_write_after) {
        return f->err;
    }
    return blk_write(f->inner, lba, count, buf);
}

static int fault_sync(blk_dev_t *dev)
{
    blk_fault_t *f = dev->priv;

    f->syncs++;
    if (f->fail_sync_after >= 0 && f->syncs > (unsigned long)f->fail_sync_after) {
        return f->err;
    }
    return blk_sync(f->inner);
}

void blk_fault_init(blk_fault_t *f, blk_dev_t *inner)
{
    memset(f, 0, sizeof(*f));
    f->inner = inner;
    f->err = BLK_ERR_IO;
    f->fail_read_after = -1;
    f->fail_write_after = -1;
    f->fail_sync_after = -1;
    f->dev = *inner;
    f->dev.name = "fault";
    f->dev.read = fault_read;
    f->dev.write = fault_write;
    f->dev.sync = fault_sync;
    f->dev.priv = f;
}
