/*
 * FatFs media access layer: maps drive numbers 0 and 1 to the blk_dev_t
 * attached to fs_vols[]. FatFs only sees RES_* codes, so the last BLK_ERR_*
 * code is kept per volume for fs.c to report.
 *
 * A device that returns BLK_ERR_NOMEDIA makes its volume gone. From then
 * on the drive reports STA_NOINIT | STA_NODISK and every transfer fails
 * with RES_NOTRDY before reaching the device. FatFs then never serves its
 * cached window, never re-mounts the drive by itself, and writes nothing
 * back. Only slot_free() in fs.c clears it, when the slot is given back.
 */
#include "fs_priv.h"

#include "diskio.h"

#include <robco/datetime.h>

fs_vol_t fs_vols[FF_VOLUMES];
uint32_t fs_change_count;

void fs_vol_lost(int vol)
{
    if (!fs_vols[vol].gone) {
        fs_vols[vol].gone = 1;
        /* A mount in progress fails instead: nothing changed for callers. */
        if (fs_vols[vol].gen != 0) {
            fs_change_count++;
        }
    }
}

/* 2026-01-01 00:00:00 in FAT format. */
#define FS_FIXED_TIME ((uint32_t)((2026u - 1980u) << 25 | 1u << 21 | 1u << 16))

/*
 * clock_get() from rtc.h, when the image links the clock. A weak
 * reference keeps the file system free of a hard dependency on the RTC
 * driver: without it the pointer is NULL. Mach-O hosts have no weak
 * undefined symbols, so there only fs_set_clock() applies.
 */
#ifdef __ELF__
extern int clock_get(datetime_t *out) __attribute__((weak));
#define DEFAULT_CLOCK clock_get
#else
#define DEFAULT_CLOCK NULL
#endif

static fs_clock_fn time_source;
static int time_source_set;

void fs_set_clock(fs_clock_fn get)
{
    time_source = get;
    time_source_set = get != NULL;
}

__attribute__((weak)) uint32_t fs_time_hook(void)
{
    fs_clock_fn get = time_source_set ? time_source : DEFAULT_CLOCK;
    datetime_t now;
    uint16_t date, time;

    if (get != NULL && get(&now) == 0 && dt_to_fat(&now, &date, &time) == 0) {
        return (uint32_t)date << 16 | time;
    }
    return FS_FIXED_TIME;
}

#if !FF_FS_READONLY && !FF_FS_NORTC
DWORD get_fattime(void);

DWORD get_fattime(void)
{
    return fs_time_hook();
}
#endif

/* The device of a drive, or NULL when it has none or its medium left. */
static blk_dev_t *drive_dev(BYTE pdrv)
{
    return pdrv < FF_VOLUMES && !fs_vols[pdrv].gone ? fs_vols[pdrv].dev : NULL;
}

static DSTATUS drive_status(BYTE pdrv)
{
    blk_dev_t *dev = drive_dev(pdrv);

    if (dev == NULL) {
        return pdrv < FF_VOLUMES && fs_vols[pdrv].gone ? STA_NOINIT | STA_NODISK : STA_NOINIT;
    }
    return (dev->flags & BLK_FLAG_READONLY) != 0 || fs_vols[pdrv].ro ? STA_PROTECT : 0;
}

DSTATUS disk_status(BYTE pdrv)
{
    return drive_status(pdrv);
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return drive_status(pdrv);
}

static DRESULT blk_result(BYTE pdrv, int rc)
{
    if (rc == 0) {
        return RES_OK;
    }
    fs_vols[pdrv].blk_err = rc;
    if (rc == BLK_ERR_NOMEDIA) {
        fs_vol_lost(pdrv);
    }
    switch (rc) {
    case BLK_ERR_RO:      return RES_WRPRT;
    case BLK_ERR_NOMEDIA: return RES_NOTRDY;
    case BLK_ERR_PARAM:   return RES_PARERR;
    default:              return RES_ERROR;
    }
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    blk_dev_t *dev = drive_dev(pdrv);

    if (dev == NULL) {
        return RES_NOTRDY;
    }
    return blk_result(pdrv, blk_read(dev, sector, count, buff));
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    blk_dev_t *dev = drive_dev(pdrv);

    if (dev == NULL) {
        return RES_NOTRDY;
    }
    if (fs_vols[pdrv].ro) {
        return RES_WRPRT;
    }
    return blk_result(pdrv, blk_write(dev, sector, count, buff));
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    blk_dev_t *dev = drive_dev(pdrv);

    if (dev == NULL) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return blk_result(pdrv, blk_sync(dev));
    case GET_SECTOR_COUNT:
        if (dev->block_count == 0) {
            return RES_PARERR;
        }
        *(LBA_t *)buff = dev->block_count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (LBA_t)dev->block_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = BLK_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
