/* Host-only block devices for tests and fs_tool. */
#ifndef BLK_FILE_H
#define BLK_FILE_H

#include <stdint.h>

#include <robco/blk.h>

/*
 * A host file as a blk_dev_t. The size is rounded down to whole blocks.
 * Returns 0, or -1 if the file cannot be opened. blk_file_close() closes it.
 */
int blk_file_open(blk_dev_t *dev, const char *path, uint32_t flags);
void blk_file_close(blk_dev_t *dev);

/*
 * Fault injection around another device. With fail_write_after = N, the
 * first N write calls go through and every later one returns err without
 * touching the inner device. Reads and syncs work the same way. -1 turns
 * faults off.
 *
 * With drop_write_after = N, the first N blocks written reach the inner
 * device and every later block is silently dropped while the call still
 * returns 0: a power cut after block N, as the medium sees it.
 */
typedef struct {
    blk_dev_t dev;      /* the wrapped device to hand out */
    blk_dev_t *inner;
    unsigned reads, writes, syncs;
    long fail_read_after, fail_write_after, fail_sync_after;
    long drop_write_after;
    uint64_t blocks_written, dropped;
    int err;
} blk_fault_t;

void blk_fault_init(blk_fault_t *f, blk_dev_t *inner);

#endif
