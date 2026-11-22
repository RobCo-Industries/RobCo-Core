#ifndef ROBCO_PART_H
#define ROBCO_PART_H

#include <stdint.h>

#include <robco/blk.h>

/* MBR partition tables. Errors are BLK_ERR_* codes from blk.h. */

#define PART_MAX 4u /* primary entries in an MBR */

#define PART_TYPE_FAT12      0x01u
#define PART_TYPE_FAT16_SM   0x04u
#define PART_TYPE_EXTENDED   0x05u
#define PART_TYPE_FAT16      0x06u
#define PART_TYPE_FAT32      0x0Bu
#define PART_TYPE_FAT32_LBA  0x0Cu
#define PART_TYPE_FAT16_LBA  0x0Eu
#define PART_TYPE_EXT_LBA    0x0Fu
#define PART_TYPE_GPT        0xEEu

typedef struct {
    blk_dev_t *dev;       /* device the entry was read from */
    uint8_t index;        /* MBR slot 0 to 3; 0 for a superfloppy */
    uint8_t type;         /* MBR type byte; 0 for a superfloppy */
    uint8_t bootable;     /* 1 if the active flag (0x80) is set */
    uint8_t superfloppy;  /* 1: FAT boot sector at block 0, no table */
    uint64_t start_lba;
    uint64_t block_count;
} part_info_t;

/*
 * Reads block 0 of dev. With an MBR, stores the used primary entries in
 * slot order, at most max of them, and returns how many it stored. Empty
 * entries and entries that do not fit the device are skipped. Extended
 * partitions are listed, not followed. With a FAT boot sector at block 0,
 * stores one superfloppy entry covering the device and returns 1. Returns 0
 * when block 0 holds neither, or a negative BLK_ERR_* code.
 */
int part_scan(blk_dev_t *dev, part_info_t *out, unsigned max);

/*
 * Fills out with a block device limited to p. Block 0 of out is
 * p->start_lba of dev, and out rejects blocks past p->block_count.
 * out keeps a pointer to p: p must stay valid and unchanged while out is
 * in use. p->dev must be dev. Returns 0 or a negative BLK_ERR_* code.
 */
int part_open(blk_dev_t *dev, const part_info_t *p, blk_dev_t *out);

#endif
