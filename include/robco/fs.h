#ifndef ROBCO_FS_H
#define ROBCO_FS_H

#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>
#include <robco/datetime.h>
#include <robco/part.h>

/*
 * FAT12, FAT16 and FAT32 over a blk_dev_t. Paths are "vol:/DIR/NAME.EXT".
 * A path without "vol:" uses the default volume, the first one mounted.
 * Names are UTF-8 and case-insensitive. No locking: one caller at a time.
 */

/* Every function returns 0 or a byte count on success, or one of these. */
enum {
    FS_OK = 0,
    FS_ERR_IO = -1,          /* the device reported an error */
    FS_ERR_TIMEOUT = -2,     /* the device did not answer in time */
    FS_ERR_NOTREADY = -3,    /* no medium, or the device is not ready */
    FS_ERR_RO = -4,          /* device or volume is read-only */
    FS_ERR_INVAL = -5,       /* bad argument or flags */
    FS_ERR_NOENT = -6,       /* no such file or directory */
    FS_ERR_EXIST = -7,       /* the name already exists */
    FS_ERR_DENIED = -8,      /* not allowed: non-empty directory, full root, read-only file */
    FS_ERR_ISDIR = -9,       /* a directory where a file was expected */
    FS_ERR_NAME = -10,       /* invalid name */
    FS_ERR_NAMETOOLONG = -11,/* path longer than FS_PATH_MAX */
    FS_ERR_NOSPC = -12,      /* volume full */
    FS_ERR_NOVOL = -13,      /* no such volume, or not mounted */
    FS_ERR_NOFS = -14,       /* no FAT file system on the device */
    FS_ERR_BUSY = -15,       /* no free volume slot, or name in use */
    FS_ERR_BADF = -16,       /* file or directory handle not open */
    FS_ERR_CORRUPT = -17,    /* inconsistent file system structures */
    FS_ERR_NOMEM = -18,      /* a work buffer could not be allocated */
    FS_ERR_MKFS = -19,       /* the device cannot hold a FAT volume */
    FS_ERR_TOOBIG = -20,     /* the file is larger than the buffer */
    FS_ERR_MOUNTED = -21,    /* the device, or blocks of it, are already mounted */
    FS_ERR_GONE = -22,       /* the medium left: unmount the volume, mount it again */
};

/*
 * Volumes mounted at the same time. Set ROBCO_FS_VOLUMES in robco_config.h
 * to change it: each slot costs about 590 bytes of BSS, mostly the FatFs
 * sector window. The default keeps the two slots RobCo-Core always had.
 */
#ifndef ROBCO_FS_VOLUMES
#define ROBCO_FS_VOLUMES 2
#endif
#define FS_VOLUMES   ((unsigned)ROBCO_FS_VOLUMES)
#define FS_VOL_MAX   7u   /* longest volume name */
#define FS_PATH_MAX  255u /* longest path, volume prefix included */
#define FS_NAME_MAX  255u /* longest name in bytes of UTF-8 */

/*
 * fs_open() flags. FS_READ or FS_WRITE or both are required. FS_TRUNC drops
 * the old content at once: use fs_write_file_atomic() to replace a file.
 */
#define FS_READ   0x01u
#define FS_WRITE  0x02u
#define FS_CREATE 0x04u /* create if missing */
#define FS_TRUNC  0x08u /* cut to 0 bytes; needs FS_WRITE */
#define FS_APPEND 0x10u /* every write goes to the end; needs FS_WRITE */
#define FS_EXCL   0x20u /* with FS_CREATE: fail with FS_ERR_EXIST if present */

#define FS_SEEK_SET 0
#define FS_SEEK_CUR 1
#define FS_SEEK_END 2

/* Caller-allocated handles. The contents are private. */
#define FS_FILE_SIZE 640u
#define FS_DIR_SIZE  128u

typedef struct {
    union {
        uint64_t align;
        unsigned char raw[FS_FILE_SIZE];
    } priv;
} fs_file_t;

typedef struct {
    union {
        uint64_t align;
        unsigned char raw[FS_DIR_SIZE];
    } priv;
} fs_dir_t;

#define FS_ATTR_RDONLY 0x01u
#define FS_ATTR_HIDDEN 0x02u
#define FS_ATTR_SYSTEM 0x04u
#define FS_ATTR_DIR    0x10u
#define FS_ATTR_ARCHIVE 0x20u

typedef struct {
    uint32_t size;               /* bytes; 0 for a directory */
    uint8_t attr;                /* FS_ATTR_* */
    uint8_t is_dir;
    uint16_t date, time;         /* FAT format */
    char name[FS_NAME_MAX + 1];  /* long name, or the 8.3 name */
    char short_name[13];         /* 8.3 name, empty if the volume has none */
    uint8_t leftover;            /* 1: a reserved work name of fs_write_file_atomic() */
} fs_stat_t;

typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t cluster_bytes;
    uint8_t fat_type;            /* 12, 16 or 32 */
} fs_statvfs_t;

#define FS_MKFS_ANY   0u /* FAT12 or FAT16 by size, FAT32 for large devices */
#define FS_MKFS_FAT   1u /* FAT12 or FAT16 only */
#define FS_MKFS_FAT32 2u /* FAT32 only */

typedef struct {
    unsigned fmt;           /* FS_MKFS_* */
    uint32_t cluster_bytes; /* 0 picks a size */
} fs_mkfs_opts_t;

const char *fs_strerror(int err);

/*
 * Volumes and removable media. Every fs_* function runs in main context,
 * never in an interrupt handler, and never blocks beyond the device calls.
 *
 * A path picks its volume by name, "tape:/GAMES/ROBCO.HOL". A path without
 * a name uses the default volume. A handle stays on the volume it was
 * opened on.
 *
 * When a device returns BLK_ERR_NOMEDIA, its volume is gone. From that
 * call on, every call on the volume, and on every handle opened on it,
 * returns FS_ERR_GONE without touching the device. fs_close() and
 * fs_closedir() still release the handle. fs_unmount() or fs_eject() then
 * frees the slot without I/O. A handle from an earlier mount of the slot
 * returns FS_ERR_BADF. The other volumes are not affected.
 */

/* fs_mount_flags() flags. */
#define FS_MOUNT_RDONLY 0x1u /* every write returns FS_ERR_RO without device I/O */

/*
 * Returns FS_ERR_MOUNTED when a mounted volume already uses dev, or a
 * part_open() device whose blocks overlap dev's on the same parent.
 * Other wrapper devices are opaque: never mount two views of one medium.
 */
int fs_mount(const char *volume, blk_dev_t *dev);
/*
 * fs_mount() with FS_MOUNT_* flags. To mount one partition, pass a
 * part_open() device.
 */
int fs_mount_flags(const char *volume, blk_dev_t *dev, unsigned flags);
/*
 * Flushes the device, then detaches the volume. If the device's sync
 * fails, returns its error and the volume stays mounted, so the caller
 * can call it again. On a gone volume it does no I/O and returns FS_OK.
 */
int fs_unmount(const char *volume);
/*
 * Detaches the volume at once, with no I/O, as if its medium left. Data
 * not yet written is lost. Open handles return FS_ERR_BADF after it.
 */
int fs_eject(const char *volume);
/*
 * Reads the volume's first block to learn whether the medium is still
 * there. Returns FS_OK, FS_ERR_GONE, FS_ERR_NOVOL or a device error.
 */
int fs_probe(const char *volume);
/*
 * Counts mounts, unmounts and lost media since boot. Poll it from the main
 * loop: when it moves, list the volumes again.
 */
uint32_t fs_changes(void);
/* Returns FS_ERR_MOUNTED for a mounted device, checked as fs_mount() does. */
int fs_mkfs(blk_dev_t *dev, const fs_mkfs_opts_t *opts);
int fs_statvfs(const char *volume, fs_statvfs_t *out);
int fs_chdir(const char *path);

int fs_open(fs_file_t *f, const char *path, unsigned flags);
int fs_read(fs_file_t *f, void *buf, size_t len);
int fs_write(fs_file_t *f, const void *buf, size_t len);
int fs_seek(fs_file_t *f, int64_t offset, int whence);
int64_t fs_tell(fs_file_t *f);
int64_t fs_size(fs_file_t *f);
int fs_sync(fs_file_t *f);
/*
 * Flushes and closes. The handle is closed even when the flush fails, and
 * on a gone volume, where it returns FS_ERR_GONE.
 */
int fs_close(fs_file_t *f);

int fs_stat(const char *path, fs_stat_t *out);
int fs_unlink(const char *path);
/*
 * FS_ERR_EXIST when to exists. Not atomic: a power cut inside it can leave
 * both names on one cluster chain.
 */
int fs_rename(const char *from, const char *to);
int fs_mkdir(const char *path);

int fs_opendir(fs_dir_t *d, const char *path);
int fs_readdir(fs_dir_t *d, fs_stat_t *out);
int fs_closedir(fs_dir_t *d);

int fs_read_file(const char *path, void *buf, size_t max, size_t *out_len);
/*
 * Replaces path with buf. After a power cut or an error, fs_atomic_recover()
 * leaves path holding either the complete previous content or the complete
 * new content: never a mix, and never nothing if path existed before. A
 * first write that did not finish leaves path absent. Read such files with
 * fs_read_file_atomic(), or call fs_atomic_recover() first, before trusting
 * them. Not guaranteed: that the new content survives a power cut before
 * the call returns FS_OK; anything when the medium corrupts sectors or
 * ignores syncs; two callers writing one path. fs_write_file_atomic() runs
 * the recovery itself first.
 *
 * Names starting with '~' or '#' are reserved for the work files: "~NAME"
 * holds new content, "#NAME" old content, next to the target NAME. A target
 * with a reserved name returns FS_ERR_NAME.
 */
int fs_write_file_atomic(const char *path, const void *buf, size_t len);
/* Finishes or undoes an interrupted fs_write_file_atomic() of path. */
int fs_atomic_recover(const char *path);
/*
 * fs_atomic_recover(), then fs_read_file(). On a read-only volume it
 * changes nothing and reads whichever file holds the content.
 */
int fs_read_file_atomic(const char *path, void *buf, size_t max, size_t *out_len);

/* fs_boot_t.slot for a card with no partition table. */
#define FS_BOOT_WHOLE 0xffu

/* The volume fs_mount_boot() chose. */
typedef struct {
    uint8_t slot;          /* MBR slot 0 to 3, or FS_BOOT_WHOLE */
    uint8_t type;          /* MBR type byte; 0 for FS_BOOT_WHOLE */
    uint8_t marker;        /* 1: holds one of the markers */
    uint8_t by_type;       /* 1: a FAT type byte or no table; 0: found in the second pass */
    uint64_t start_lba;    /* first block on the card; 0 for FS_BOOT_WHOLE */
    uint64_t block_count;
    int last_err;          /* last part_scan() or fs_mount() error, 0 if none */
    part_info_t part;      /* private */
    blk_dev_t dev;         /* private: the mounted partition device */
} fs_boot_t;

/*
 * Finds the boot volume on dev and mounts it as `volume`. Every RobCo
 * stage uses this rule, so they all pick the same volume:
 *   1. Read the MBR. A card with no table and a FAT boot sector at block 0
 *      is one volume, the whole card.
 *   2. Try the entries with a FAT type byte in slot order, then the other
 *      entries in slot order, because some tools write a wrong type byte.
 *      Extended and GPT protective entries are never mounted.
 *   3. With markers, paths such as "/ROBCO/MFBOOT.SYS", choose the first
 *      volume that mounts and holds one of them as a regular file of at
 *      least one byte. If none holds one, choose the first that mounts.
 *      Without markers, choose the first that mounts.
 * On every return, either exactly one volume is mounted as `volume` and
 * out describes it, or this call left nothing mounted:
 *   PART_BOOT_OK: the chosen volume is mounted.
 *   PART_BOOT_ERR_RELEASE: a volume that held no marker did not unmount,
 *     because the device failed to flush. The search stops there. That
 *     volume stays mounted, out describes it and last_err holds the
 *     fs_unmount() error. Retry fs_unmount(volume) or use it.
 *   Any other PART_BOOT_ERR_* code: nothing is mounted.
 * out must stay valid and unchanged while the volume is mounted: the
 * device lives in it. When `volume` is mounted, or out holds a mounted
 * device, returns PART_BOOT_ERR_BUSY and leaves out as it is. Unmount
 * with fs_unmount(volume).
 */
int fs_mount_boot(const char *volume, blk_dev_t *dev, const char *const *markers,
                  unsigned n_markers, fs_boot_t *out);

/*
 * Timestamp source, FAT format: date << 16 | time. The weak default uses
 * the clock from fs_set_clock(), or clock_get() from rtc.h when the image
 * links it. When the clock is not set, it returns 2026-01-01 00:00.
 */
uint32_t fs_time_hook(void);

/* Returns 0 and the time, like clock_get(), or a negative code when not set. */
typedef int (*fs_clock_fn)(datetime_t *out);
/*
 * Sets the clock fs_time_hook() reads. NULL goes back to the default:
 * clock_get() when linked, else the fixed time.
 */
void fs_set_clock(fs_clock_fn get);

#endif
