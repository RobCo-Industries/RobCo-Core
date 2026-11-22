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
    FATFS fs;
} fs_vol_t;

extern fs_vol_t fs_vols[FF_VOLUMES];

#endif
