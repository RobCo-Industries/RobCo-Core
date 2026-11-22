#ifndef ROBCO_BLK_H
#define ROBCO_BLK_H

#include <stddef.h>
#include <stdint.h>

/* Block device interface. Storage drivers fill one in. File systems use it. */

#define BLK_SECTOR_SIZE 512u

/* Every blk function returns 0 on success or one of these. */
#define BLK_ERR_IO      (-1) /* the device reported an error */
#define BLK_ERR_TIMEOUT (-2) /* the device did not answer in time */
#define BLK_ERR_RANGE   (-3) /* block number or count out of range */
#define BLK_ERR_NOMEDIA (-4) /* no card or medium present */
#define BLK_ERR_RO      (-5) /* the device is read-only */
#define BLK_ERR_PARAM   (-6) /* bad argument */

#define BLK_FLAG_READONLY  0x1u
#define BLK_FLAG_REMOVABLE 0x2u

typedef struct blk_dev blk_dev_t;

struct blk_dev {
    const char *name;     /* short name for messages, for example "sd0" */
    uint32_t block_size;  /* bytes per block, BLK_SECTOR_SIZE today */
    uint64_t block_count; /* number of blocks, 0 if unknown */
    uint32_t flags;       /* BLK_FLAG_* */
    /* Transfer count whole blocks starting at lba. buf holds count * block_size bytes. */
    int (*read)(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*sync)(blk_dev_t *dev); /* flush device buffers, may be NULL */
    void *priv;           /* driver data */
};

/* Checked wrappers. They validate the arguments and the range, then call the driver. */

static inline int blk_check(const blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (dev == NULL || buf == NULL || count == 0) {
        return BLK_ERR_PARAM;
    }
    if (dev->block_count != 0 && (lba >= dev->block_count || count > dev->block_count - lba)) {
        return BLK_ERR_RANGE;
    }
    return 0;
}

static inline int blk_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    int rc = blk_check(dev, lba, count, buf);
    if (rc != 0) {
        return rc;
    }
    if (dev->read == NULL) {
        return BLK_ERR_PARAM;
    }
    return dev->read(dev, lba, count, buf);
}

static inline int blk_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    int rc = blk_check(dev, lba, count, buf);
    if (rc != 0) {
        return rc;
    }
    if ((dev->flags & BLK_FLAG_READONLY) != 0) {
        return BLK_ERR_RO;
    }
    if (dev->write == NULL) {
        return BLK_ERR_PARAM;
    }
    return dev->write(dev, lba, count, buf);
}

static inline int blk_sync(blk_dev_t *dev)
{
    if (dev == NULL) {
        return BLK_ERR_PARAM;
    }
    return dev->sync != NULL ? dev->sync(dev) : 0;
}

static inline const char *blk_strerror(int err)
{
    switch (err) {
    case 0:               return "ok";
    case BLK_ERR_IO:      return "io";
    case BLK_ERR_TIMEOUT: return "timeout";
    case BLK_ERR_RANGE:   return "range";
    case BLK_ERR_NOMEDIA: return "nomedia";
    case BLK_ERR_RO:      return "readonly";
    case BLK_ERR_PARAM:   return "param";
    default:              return "unknown";
    }
}

#endif
