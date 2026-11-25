/* Shared state between the fs API (fs.c) and the FatFs disk glue (fs_glue.c). */
#ifndef ROBCO_FS_PRIV_H
#define ROBCO_FS_PRIV_H

#include <robco/blk.h>
#include <robco/fs.h>

#include "ff.h"

typedef struct {
    char name[FS_VOL_MAX + 1]; /* empty while not mounted */
    blk_dev_t *dev;            /* set while mounted or during fs_mkfs */
    int blk_err;               /* last BLK_ERR_* from the device, 0 if none */
    uint32_t gen;              /* this mount's number, 0 while not mounted */
    uint8_t gone;              /* 1: the device reported no medium */
    uint8_t ro;                /* 1: mounted with FS_MOUNT_RDONLY */
    FATFS fs;
} fs_vol_t;

_Static_assert(FS_VOLUMES == FF_VOLUMES, "fs.h and ffconf.h disagree on the volume count");

extern fs_vol_t fs_vols[FF_VOLUMES];
extern uint32_t fs_change_count;

/* Marks slot vol gone, once. The glue and fs.c call it. */
void fs_vol_lost(int vol);

#endif
