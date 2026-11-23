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

/*
 * When dev came from part_open(), returns its parent device and stores the
 * partition's first block on the parent in *start_lba. Returns NULL for any
 * other device. start_lba may be NULL.
 */
blk_dev_t *part_parent_of(const blk_dev_t *dev, uint64_t *start_lba);

/*
 * The boot volume rule. fs_mount_boot() in fs.h runs it on a card. The
 * functions below hold its decisions and touch no hardware.
 */

/* Results of fs_mount_boot(), and names from part_boot_strerror(). */
#define PART_BOOT_OK         0  /* "ok" */
#define PART_BOOT_ERR_DEVICE 1  /* "nodev": no device, or block 0 does not read */
#define PART_BOOT_ERR_NOFAT  2  /* "nofat": no FAT entry, and no other entry mounts */
#define PART_BOOT_ERR_GPT    3  /* "gpt": a GPT card, not supported */
#define PART_BOOT_ERR_MOUNT  4  /* "mount": FAT entries, none mounts */
#define PART_BOOT_ERR_ARG    5  /* "arg": bad argument */
#define PART_BOOT_ERR_BUSY   6  /* "busy": the volume name, or the result's device, is mounted */
#define PART_BOOT_ERR_RELEASE 7 /* "release": a probed volume did not unmount, still mounted */

/* part_boot_class() results. */
#define PART_BOOT_NEVER 0  /* extended or GPT protective entry: never mounted */
#define PART_BOOT_FAT   1  /* a FAT type byte, or a card with no table */
#define PART_BOOT_OTHER 2  /* any other type: tried after every FAT entry */

int part_boot_class(const part_info_t *p);

/*
 * Stores in order[] the indexes into parts[] in the order to try them:
 * PART_BOOT_FAT entries in slot order, then PART_BOOT_OTHER entries in
 * slot order. n is at most PART_MAX. Returns how many it stored.
 */
unsigned part_boot_order(const part_info_t *parts, unsigned n, uint8_t order[PART_MAX]);

/* What fs_mount_boot() found on one entry it tried. */
#define PART_PROBE_NOMOUNT 0  /* did not mount */
#define PART_PROBE_MOUNTED 1  /* mounted, holds no marker */
#define PART_PROBE_MARKER  2  /* mounted and holds a marker */

/*
 * The entry to use, given the probe results of n entries in try order:
 * the first PART_PROBE_MARKER, else the first PART_PROBE_MOUNTED.
 * Returns its position in probe[], or -1 when none mounted.
 */
int part_boot_pick(const uint8_t *probe, unsigned n);

/*
 * Why no volume was chosen. scan is the part_scan() result and parts[]
 * its entries. Returns PART_BOOT_ERR_DEVICE for a negative scan, then
 * PART_BOOT_ERR_MOUNT when a PART_BOOT_FAT entry exists, then
 * PART_BOOT_ERR_GPT when a GPT protective entry exists, else
 * PART_BOOT_ERR_NOFAT.
 */
int part_boot_failure(int scan, const part_info_t *parts);

/* "ok", "nodev", "nofat", "gpt", "mount", "arg", "busy", "release",
 * or "unknown". */
const char *part_boot_strerror(int rc);

#endif
