#ifndef ROBCO_FS_H
#define ROBCO_FS_H

#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>
#include <robco/datetime.h>

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
};

#define FS_VOLUMES   2u   /* volumes mounted at the same time */
#define FS_VOL_MAX   7u   /* longest volume name */
#define FS_PATH_MAX  255u /* longest path, volume prefix included */
#define FS_NAME_MAX  255u /* longest name in bytes of UTF-8 */

/* fs_open() flags. FS_READ or FS_WRITE or both are required. */
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

int fs_mount(const char *volume, blk_dev_t *dev);
/*
 * Flushes the device, then detaches the volume. If the device's sync
 * fails, returns its error and the volume stays mounted, so the caller
 * can call it again.
 */
int fs_unmount(const char *volume);
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
/* Flushes and closes. The handle is closed even when the flush fails. */
int fs_close(fs_file_t *f);

int fs_stat(const char *path, fs_stat_t *out);
int fs_unlink(const char *path);
int fs_rename(const char *from, const char *to);
int fs_mkdir(const char *path);

int fs_opendir(fs_dir_t *d, const char *path);
int fs_readdir(fs_dir_t *d, fs_stat_t *out);
int fs_closedir(fs_dir_t *d);

int fs_read_file(const char *path, void *buf, size_t max, size_t *out_len);
int fs_write_file_atomic(const char *path, const void *buf, size_t len);

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
