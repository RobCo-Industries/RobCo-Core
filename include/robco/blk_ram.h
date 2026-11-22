#ifndef ROBCO_BLK_RAM_H
#define ROBCO_BLK_RAM_H

#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>

/*
 * RAM disk: a blk_dev_t over a caller buffer. size is rounded down to whole
 * blocks. flags takes BLK_FLAG_*. The buffer must outlive the device.
 * Returns 0, or BLK_ERR_PARAM if dev or buf is NULL or size is under one block.
 */
int blk_ram_init(blk_dev_t *dev, void *buf, size_t size, uint32_t flags);

#endif
