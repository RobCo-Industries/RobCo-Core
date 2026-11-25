/* Shared state between the fs API (fs.c) and the FatFs disk glue (fs_glue.c). */
#ifndef ROBCO_FS_PRIV_H
#define ROBCO_FS_PRIV_H

#include <robco/blk.h>
#include <robco/fs.h>

#include "ff.h"

/*
 * One volume slot. fs.c sets the fields only through slot_take() and the
 * mount path, and clears all of them at once in slot_free(), which every
 * mount failure, unmount, eject and fs_mkfs() exit calls.
 */
typedef struct {
    char name[FS_VOL_MAX + 1]; /* set by a mount that succeeds, empty otherwise */
    blk_dev_t *dev;            /* set by slot_take(): mounted, mounting or formatting */
    int blk_err;               /* last BLK_ERR_* from the device, 0 if none; reset per call */
    uint32_t gen;              /* this mount's number, set by a mount that succeeds, else 0 */
    uint8_t gone;              /* 1: the device reported no medium; set by fs_vol_lost() */
    uint8_t ro;                /* 1: FS_MOUNT_RDONLY; set by slot_take() */
    FATFS fs;                  /* FatFs work area, registered by f_mount() */
} fs_vol_t;

_Static_assert(FS_VOLUMES == FF_VOLUMES, "fs.h and ffconf.h disagree on the volume count");

extern fs_vol_t fs_vols[FF_VOLUMES];
extern uint32_t fs_change_count;

/* Marks slot vol gone, once. The glue and fs.c call it. */
void fs_vol_lost(int vol);

#endif
