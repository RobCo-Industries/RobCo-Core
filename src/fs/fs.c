/*
 * The library's file API over FatFs. Paths "vol:rest" become "N:rest" for
 * FatFs, where N is the slot of the mounted volume. Handles keep FatFs
 * objects in caller memory; the asserts below keep fs.h large enough.
 */
#include <robco/klibc.h>
#include <robco/part.h>

#include "fs_priv.h"

#include "diskio.h"

#define FILE_MAGIC 0x46494C45u /* "FILE" */
#define DIR_MAGIC  0x44495230u /* "DIR0" */
#define IO_MAX     0x7FFFFFFFu /* largest count fs_read and fs_write return */

typedef struct {
    FIL fil;
    uint32_t magic;
    uint8_t vol;
    uint8_t flags;
} file_priv_t;

typedef struct {
    DIR dir;
    uint32_t magic;
    uint8_t vol;
} dir_priv_t;

_Static_assert(sizeof(file_priv_t) <= sizeof(((fs_file_t *)0)->priv), "FS_FILE_SIZE too small");
_Static_assert(sizeof(dir_priv_t) <= sizeof(((fs_dir_t *)0)->priv), "FS_DIR_SIZE too small");
_Static_assert(_Alignof(file_priv_t) <= _Alignof(fs_file_t), "fs_file_t alignment");
_Static_assert(_Alignof(dir_priv_t) <= _Alignof(fs_dir_t), "fs_dir_t alignment");

static int default_vol = -1;

static file_priv_t *file_of(fs_file_t *f)
{
    file_priv_t *p = (file_priv_t *)(void *)f;

    return f != NULL && p->magic == FILE_MAGIC ? p : NULL;
}

static dir_priv_t *dir_of(fs_dir_t *d)
{
    dir_priv_t *p = (dir_priv_t *)(void *)d;

    return d != NULL && p->magic == DIR_MAGIC ? p : NULL;
}

static int from_blk(int rc)
{
    switch (rc) {
    case BLK_ERR_TIMEOUT: return FS_ERR_TIMEOUT;
    case BLK_ERR_NOMEDIA: return FS_ERR_NOTREADY;
    case BLK_ERR_RO:      return FS_ERR_RO;
    case BLK_ERR_PARAM:   return FS_ERR_INVAL;
    case BLK_ERR_RANGE:   return FS_ERR_CORRUPT;
    default:              return FS_ERR_IO;
    }
}

/* vol is the slot the call used, or -1. */
static int from_fr(FRESULT fr, int vol)
{
    int blk = vol >= 0 ? fs_vols[vol].blk_err : 0;

    switch (fr) {
    case FR_OK:                  return FS_OK;
    case FR_DISK_ERR:            return blk != 0 ? from_blk(blk) : FS_ERR_IO;
    case FR_INT_ERR:             return FS_ERR_CORRUPT;
    case FR_NOT_READY:           return blk != 0 ? from_blk(blk) : FS_ERR_NOTREADY;
    case FR_NO_FILE:             return FS_ERR_NOENT;
    case FR_NO_PATH:             return FS_ERR_NOENT;
    case FR_INVALID_NAME:        return FS_ERR_NAME;
    case FR_DENIED:              return FS_ERR_DENIED;
    case FR_EXIST:               return FS_ERR_EXIST;
    case FR_INVALID_OBJECT:      return FS_ERR_BADF;
    case FR_WRITE_PROTECTED:     return FS_ERR_RO;
    case FR_INVALID_DRIVE:       return FS_ERR_NOVOL;
    case FR_NOT_ENABLED:         return FS_ERR_NOVOL;
    case FR_NO_FILESYSTEM:       return FS_ERR_NOFS;
    case FR_MKFS_ABORTED:        return FS_ERR_MKFS;
    case FR_TIMEOUT:             return FS_ERR_TIMEOUT;
    case FR_LOCKED:              return FS_ERR_BUSY;
    case FR_NOT_ENOUGH_CORE:     return FS_ERR_NOMEM;
    case FR_TOO_MANY_OPEN_FILES: return FS_ERR_BUSY;
    case FR_INVALID_PARAMETER:   return FS_ERR_INVAL;
    default:                     return FS_ERR_IO;
    }
}

const char *fs_strerror(int err)
{
    switch (err) {
    case FS_OK:              return "ok";
    case FS_ERR_IO:          return "i/o error";
    case FS_ERR_TIMEOUT:     return "device timeout";
    case FS_ERR_NOTREADY:    return "device not ready";
    case FS_ERR_RO:          return "read-only";
    case FS_ERR_INVAL:       return "invalid argument";
    case FS_ERR_NOENT:       return "no such file or directory";
    case FS_ERR_EXIST:       return "already exists";
    case FS_ERR_DENIED:      return "access denied";
    case FS_ERR_ISDIR:       return "is a directory";
    case FS_ERR_NAME:        return "invalid name";
    case FS_ERR_NAMETOOLONG: return "path too long";
    case FS_ERR_NOSPC:       return "no space left";
    case FS_ERR_NOVOL:       return "no such volume";
    case FS_ERR_NOFS:        return "no FAT file system";
    case FS_ERR_BUSY:        return "busy";
    case FS_ERR_BADF:        return "handle not open";
    case FS_ERR_CORRUPT:     return "file system corrupt";
    case FS_ERR_NOMEM:       return "out of memory";
    case FS_ERR_MKFS:        return "cannot format";
    case FS_ERR_TOOBIG:      return "file too big";
    case FS_ERR_MOUNTED:     return "device already mounted";
    default:                 return "unknown error";
    }
}

/* "N:" for FatFs. */
static void drive_prefix(char *out, int vol)
{
    out[0] = (char)('0' + vol);
    out[1] = ':';
    out[2] = '\0';
}

static int find_vol(const char *name, size_t len)
{
    int i;

    for (i = 0; i < (int)FF_VOLUMES; i++) {
        if (fs_vols[i].name[0] != '\0' && k_strlen(fs_vols[i].name) == len &&
            k_memcmp(fs_vols[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Rewrites path for FatFs into out, which holds FS_PATH_MAX + 1 bytes.
 * Returns the volume slot or a negative FS_ERR_* code.
 */
static int resolve(const char *path, char *out)
{
    const char *rest = path;
    size_t i, len;
    int vol = default_vol;

    if (path == NULL) {
        return FS_ERR_INVAL;
    }
    len = k_strlen(path);
    if (len > FS_PATH_MAX) {
        return FS_ERR_NAMETOOLONG;
    }
    for (i = 0; i < len && path[i] != '/' && path[i] != '\\'; i++) {
        if (path[i] == ':') {
            vol = find_vol(path, i);
            rest = path + i + 1;
            if (vol < 0) {
                return FS_ERR_NOVOL;
            }
            break;
        }
    }
    if (vol < 0) {
        return FS_ERR_NOVOL;
    }
    len = k_strlen(rest);
    if (len + 2 > FS_PATH_MAX) {
        return FS_ERR_NAMETOOLONG;
    }
    drive_prefix(out, vol);
    k_memcpy(out + 2, rest, len + 1);
    fs_vols[vol].blk_err = 0;
    return vol;
}

static int is_root(const char *fpath)
{
    const char *p = fpath + 2;

    if (*p == '\0') {
        return 0; /* the current directory, not always the root */
    }
    while (*p == '/' || *p == '\\') {
        p++;
    }
    return *p == '\0';
}

static int valid_vol_name(const char *name)
{
    size_t i, len;

    if (name == NULL) {
        return 0;
    }
    len = k_strlen(name);
    if (len == 0 || len > FS_VOL_MAX) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (name[i] == ':' || name[i] == '/' || name[i] == '\\') {
            return 0;
        }
    }
    return 1;
}

/* The device under any part_open() layers, and dev's first block on it. */
static blk_dev_t *dev_root(blk_dev_t *dev, uint64_t *start)
{
    blk_dev_t *parent;
    uint64_t off;
    int depth;

    *start = 0;
    for (depth = 0; depth < 8; depth++) {
        parent = part_parent_of(dev, &off);
        if (parent == NULL) {
            break;
        }
        *start += off;
        dev = parent;
    }
    return dev;
}

/* 1 when a and b reach the same medium through overlapping blocks. */
static int devs_overlap(blk_dev_t *a, blk_dev_t *b)
{
    uint64_t sa, sb, ea, eb;

    if (a == b) {
        return 1;
    }
    if (dev_root(a, &sa) != dev_root(b, &sb)) {
        return 0;
    }
    /* A size of 0 means unknown: the device may reach the end. */
    ea = a->block_count != 0 ? sa + a->block_count : UINT64_MAX;
    eb = b->block_count != 0 ? sb + b->block_count : UINT64_MAX;
    return sa < eb && sb < ea;
}

/* FS_ERR_MOUNTED when a volume, mounted or being formatted, uses dev. */
static int check_dev_free(blk_dev_t *dev)
{
    int i;

    for (i = 0; i < (int)FF_VOLUMES; i++) {
        if (fs_vols[i].dev != NULL && devs_overlap(fs_vols[i].dev, dev)) {
            return FS_ERR_MOUNTED;
        }
    }
    return FS_OK;
}

int fs_mount(const char *volume, blk_dev_t *dev)
{
    char drv[3];
    int i, slot = -1, rc;

    if (!valid_vol_name(volume) || dev == NULL || dev->block_size != BLK_SECTOR_SIZE) {
        return FS_ERR_INVAL;
    }
    if (find_vol(volume, k_strlen(volume)) >= 0) {
        return FS_ERR_BUSY;
    }
    /* Two FatFs volumes over one medium keep separate caches and corrupt it. */
    rc = check_dev_free(dev);
    if (rc != FS_OK) {
        return rc;
    }
    for (i = 0; i < (int)FF_VOLUMES && slot < 0; i++) {
        if (fs_vols[i].dev == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        return FS_ERR_BUSY;
    }
    fs_vols[slot].dev = dev;
    fs_vols[slot].blk_err = 0;
    drive_prefix(drv, slot);
    rc = from_fr(f_mount(&fs_vols[slot].fs, drv, 1), slot);
    if (rc != FS_OK) {
        f_mount(NULL, drv, 0);
        fs_vols[slot].dev = NULL;
        return rc;
    }
    k_strncpy(fs_vols[slot].name, volume, sizeof(fs_vols[slot].name));
    if (default_vol < 0) {
        default_vol = slot;
    }
    return FS_OK;
}

int fs_unmount(const char *volume)
{
    char drv[3];
    int i, vol, rc;

    if (!valid_vol_name(volume)) {
        return FS_ERR_INVAL;
    }
    vol = find_vol(volume, k_strlen(volume));
    if (vol < 0) {
        return FS_ERR_NOVOL;
    }
    /* Flush first: on failure the volume stays mounted for a retry. */
    rc = blk_sync(fs_vols[vol].dev);
    if (rc != 0) {
        return from_blk(rc);
    }
    drive_prefix(drv, vol);
    f_mount(NULL, drv, 0);
    fs_vols[vol].dev = NULL;
    fs_vols[vol].name[0] = '\0';
    if (default_vol == vol) {
        default_vol = -1;
        for (i = 0; i < (int)FF_VOLUMES; i++) {
            if (fs_vols[i].name[0] != '\0') {
                default_vol = i;
                break;
            }
        }
    }
    return FS_OK;
}

int fs_statvfs(const char *volume, fs_statvfs_t *out)
{
    char drv[3];
    DWORD nfree;
    FATFS *fs;
    uint64_t cl;
    int vol, rc;

    if (out == NULL || !valid_vol_name(volume)) {
        return FS_ERR_INVAL;
    }
    vol = find_vol(volume, k_strlen(volume));
    if (vol < 0) {
        return FS_ERR_NOVOL;
    }
#if FF_FS_READONLY
    /* FatFs builds f_getfree() only with write support. */
    (void)drv;
    (void)nfree;
    (void)fs;
    (void)cl;
    (void)rc;
    k_memset(out, 0, sizeof(*out));
    return FS_ERR_RO;
#else
    fs_vols[vol].blk_err = 0;
    drive_prefix(drv, vol);
    rc = from_fr(f_getfree(drv, &nfree, &fs), vol);
    if (rc != FS_OK) {
        return rc;
    }
    cl = (uint64_t)fs->csize * BLK_SECTOR_SIZE;
    out->cluster_bytes = (uint32_t)cl;
    out->total_bytes = (uint64_t)(fs->n_fatent - 2) * cl;
    out->free_bytes = (uint64_t)nfree * cl;
    out->fat_type = fs->fs_type == FS_FAT12 ? 12 : fs->fs_type == FS_FAT16 ? 16 : 32;
    return FS_OK;
#endif
}

int fs_chdir(const char *path)
{
    char p[FS_PATH_MAX + 1];
    int vol = resolve(path, p), rc;

    if (vol < 0) {
        return vol;
    }
    rc = from_fr(f_chdir(p), vol);
    if (rc == FS_OK) {
        default_vol = vol;
    }
    return rc;
}

static void fill_stat(fs_stat_t *out, const FILINFO *fi)
{
    k_memset(out, 0, sizeof(*out));
    out->size = (uint32_t)fi->fsize;
    out->attr = fi->fattrib;
    out->is_dir = (fi->fattrib & AM_DIR) != 0;
    out->date = fi->fdate;
    out->time = fi->ftime;
    k_strncpy(out->name, fi->fname[0] != '\0' ? fi->fname : fi->altname, sizeof(out->name) - 1);
    k_strncpy(out->short_name, fi->altname, sizeof(out->short_name) - 1);
}

int fs_stat(const char *path, fs_stat_t *out)
{
    char p[FS_PATH_MAX + 1];
    FILINFO fi;
    int vol, rc;

    if (out == NULL) {
        return FS_ERR_INVAL;
    }
    vol = resolve(path, p);
    if (vol < 0) {
        return vol;
    }
    if (is_root(p)) {
        k_memset(out, 0, sizeof(*out));
        out->attr = FS_ATTR_DIR;
        out->is_dir = 1;
        out->name[0] = '/';
        return FS_OK;
    }
    rc = from_fr(f_stat(p, &fi), vol);
    if (rc == FS_OK) {
        fill_stat(out, &fi);
    }
    return rc;
}

static int is_dir_path(const char *fpath)
{
    FILINFO fi;

    return is_root(fpath) || (f_stat(fpath, &fi) == FR_OK && (fi.fattrib & AM_DIR) != 0);
}

int fs_open(fs_file_t *f, const char *path, unsigned flags)
{
    char p[FS_PATH_MAX + 1];
    file_priv_t *fp = (file_priv_t *)(void *)f;
    BYTE mode = 0;
    FRESULT fr;
    int vol, rc;

    if (f == NULL) {
        return FS_ERR_INVAL;
    }
    k_memset(f, 0, sizeof(*f));
    if ((flags & (FS_READ | FS_WRITE)) == 0 ||
        (flags & ~(FS_READ | FS_WRITE | FS_CREATE | FS_TRUNC | FS_APPEND | FS_EXCL)) != 0 ||
        ((flags & (FS_TRUNC | FS_APPEND | FS_CREATE)) != 0 && (flags & FS_WRITE) == 0) ||
        ((flags & FS_EXCL) != 0 && (flags & FS_CREATE) == 0)) {
        return FS_ERR_INVAL;
    }
    vol = resolve(path, p);
    if (vol < 0) {
        return vol;
    }
#if FF_FS_READONLY
    if ((flags & FS_WRITE) != 0) {
        return FS_ERR_RO;
    }
#endif
    if ((flags & FS_READ) != 0) {
        mode |= FA_READ;
    }
    if ((flags & FS_WRITE) != 0) {
        mode |= FA_WRITE;
    }
    if ((flags & FS_EXCL) != 0) {
        mode |= FA_CREATE_NEW;
    } else if ((flags & FS_CREATE) != 0) {
        mode |= (flags & FS_TRUNC) != 0 ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
    }
    if (is_root(p)) {
        return FS_ERR_ISDIR;
    }
    fr = f_open(&fp->fil, p, mode);
    if ((fr == FR_NO_FILE || fr == FR_DENIED) && is_dir_path(p)) {
        return FS_ERR_ISDIR;
    }
    rc = from_fr(fr, vol);
    if (rc != FS_OK) {
        return rc;
    }
#if !FF_FS_READONLY
    if ((flags & FS_TRUNC) != 0 && (flags & FS_CREATE) == 0) {
        rc = from_fr(f_truncate(&fp->fil), vol);
        if (rc != FS_OK) {
            f_close(&fp->fil);
            return rc;
        }
    }
#endif
    fp->magic = FILE_MAGIC;
    fp->vol = (uint8_t)vol;
    fp->flags = (uint8_t)flags;
    return FS_OK;
}

int fs_read(fs_file_t *f, void *buf, size_t len)
{
    file_priv_t *fp = file_of(f);
    UINT br = 0;
    int rc;

    if (fp == NULL) {
        return FS_ERR_BADF;
    }
    if (buf == NULL && len != 0) {
        return FS_ERR_INVAL;
    }
    if ((fp->flags & FS_READ) == 0) {
        return FS_ERR_DENIED;
    }
    if (len > IO_MAX) {
        len = IO_MAX;
    }
    fs_vols[fp->vol].blk_err = 0;
    rc = from_fr(f_read(&fp->fil, buf, (UINT)len, &br), fp->vol);
    return rc != FS_OK ? rc : (int)br;
}

#if !FF_FS_READONLY
/* Writes all of len or fails. FatFs reports a full volume as a short write. */
static int write_all(file_priv_t *fp, const void *buf, UINT len)
{
    UINT bw = 0;
    int rc = from_fr(f_write(&fp->fil, buf, len, &bw), fp->vol);

    if (rc != FS_OK) {
        return rc;
    }
    return bw == len ? (int)bw : bw == 0 ? FS_ERR_NOSPC : (int)bw;
}
#endif

int fs_write(fs_file_t *f, const void *buf, size_t len)
{
#if FF_FS_READONLY
    (void)buf;
    (void)len;
    return file_of(f) == NULL ? FS_ERR_BADF : FS_ERR_RO;
#else
    file_priv_t *fp = file_of(f);
    int rc;

    if (fp == NULL) {
        return FS_ERR_BADF;
    }
    if (buf == NULL && len != 0) {
        return FS_ERR_INVAL;
    }
    if ((fp->flags & FS_WRITE) == 0) {
        return FS_ERR_DENIED;
    }
    if (len == 0) {
        return 0;
    }
    if (len > IO_MAX) {
        len = IO_MAX;
    }
    fs_vols[fp->vol].blk_err = 0;
    if ((fp->flags & FS_APPEND) != 0) {
        rc = from_fr(f_lseek(&fp->fil, f_size(&fp->fil)), fp->vol);
        if (rc != FS_OK) {
            return rc;
        }
    }
    return write_all(fp, buf, (UINT)len);
#endif
}

int fs_seek(fs_file_t *f, int64_t offset, int whence)
{
    file_priv_t *fp = file_of(f);
    int64_t base, target;
    FSIZE_t size;
    int rc;

    if (fp == NULL) {
        return FS_ERR_BADF;
    }
    size = f_size(&fp->fil);
    switch (whence) {
    case FS_SEEK_SET: base = 0; break;
    case FS_SEEK_CUR: base = (int64_t)f_tell(&fp->fil); break;
    case FS_SEEK_END: base = (int64_t)size; break;
    default:          return FS_ERR_INVAL;
    }
    if ((offset > 0 && offset > INT64_C(0xFFFFFFFF) - base) || base + offset < 0) {
        return FS_ERR_INVAL;
    }
    target = base + offset;
    fs_vols[fp->vol].blk_err = 0;
#if !FF_FS_READONLY
    if ((uint64_t)target > size && (fp->flags & FS_WRITE) != 0) {
        /* Extend with zeros now, so the gap reads back as zeros. */
        static const unsigned char zeros[64];

        rc = from_fr(f_lseek(&fp->fil, size), fp->vol);
        while (rc == FS_OK && f_tell(&fp->fil) < (FSIZE_t)target) {
            uint64_t left = (uint64_t)target - f_tell(&fp->fil);

            rc = write_all(fp, zeros, (UINT)(left < sizeof(zeros) ? left : sizeof(zeros)));
            rc = rc < 0 ? rc : FS_OK;
        }
        return rc;
    }
#endif
    /* Without FS_WRITE, FatFs stops at the end of the file. */
    rc = from_fr(f_lseek(&fp->fil, (FSIZE_t)target), fp->vol);
    return rc;
}

int64_t fs_tell(fs_file_t *f)
{
    file_priv_t *fp = file_of(f);

    return fp == NULL ? FS_ERR_BADF : (int64_t)f_tell(&fp->fil);
}

int64_t fs_size(fs_file_t *f)
{
    file_priv_t *fp = file_of(f);

    return fp == NULL ? FS_ERR_BADF : (int64_t)f_size(&fp->fil);
}

int fs_sync(fs_file_t *f)
{
    file_priv_t *fp = file_of(f);

    if (fp == NULL) {
        return FS_ERR_BADF;
    }
#if FF_FS_READONLY
    return FS_OK;
#else
    fs_vols[fp->vol].blk_err = 0;
    return from_fr(f_sync(&fp->fil), fp->vol);
#endif
}

int fs_close(fs_file_t *f)
{
    file_priv_t *fp = file_of(f);
    int rc;

    if (fp == NULL) {
        return FS_ERR_BADF;
    }
    fs_vols[fp->vol].blk_err = 0;
    rc = from_fr(f_close(&fp->fil), fp->vol);
    fp->magic = 0;
    return rc;
}

int fs_unlink(const char *path)
{
    char p[FS_PATH_MAX + 1];
    int vol = resolve(path, p);

    if (vol < 0) {
        return vol;
    }
#if FF_FS_READONLY
    return FS_ERR_RO;
#else
    return from_fr(f_unlink(p), vol);
#endif
}

int fs_rename(const char *from, const char *to)
{
    char a[FS_PATH_MAX + 1], b[FS_PATH_MAX + 1];
    int va = resolve(from, a), vb;

    if (va < 0) {
        return va;
    }
    vb = resolve(to, b);
    if (vb < 0) {
        return vb;
    }
    if (va != vb) {
        return FS_ERR_INVAL;
    }
#if FF_FS_READONLY
    return FS_ERR_RO;
#else
    return from_fr(f_rename(a, b), va);
#endif
}

int fs_mkdir(const char *path)
{
    char p[FS_PATH_MAX + 1];
    int vol = resolve(path, p);

    if (vol < 0) {
        return vol;
    }
#if FF_FS_READONLY
    return FS_ERR_RO;
#else
    return from_fr(f_mkdir(p), vol);
#endif
}

int fs_opendir(fs_dir_t *d, const char *path)
{
    char p[FS_PATH_MAX + 1];
    dir_priv_t *dp = (dir_priv_t *)(void *)d;
    int vol, rc;

    if (d == NULL) {
        return FS_ERR_INVAL;
    }
    k_memset(d, 0, sizeof(*d));
    vol = resolve(path, p);
    if (vol < 0) {
        return vol;
    }
    rc = from_fr(f_opendir(&dp->dir, p), vol);
    if (rc == FS_OK) {
        dp->magic = DIR_MAGIC;
        dp->vol = (uint8_t)vol;
    }
    return rc;
}

int fs_readdir(fs_dir_t *d, fs_stat_t *out)
{
    dir_priv_t *dp = dir_of(d);
    FILINFO fi;
    int rc;

    if (dp == NULL) {
        return FS_ERR_BADF;
    }
    if (out == NULL) {
        return FS_ERR_INVAL;
    }
    fs_vols[dp->vol].blk_err = 0;
    for (;;) {
        rc = from_fr(f_readdir(&dp->dir, &fi), dp->vol);
        if (rc != FS_OK) {
            return rc;
        }
        if (fi.fname[0] == '\0') {
            return 0;
        }
        if (fi.fname[0] == '.' && (fi.fname[1] == '\0' || (fi.fname[1] == '.' && fi.fname[2] == '\0'))) {
            continue;
        }
        fill_stat(out, &fi);
        return 1;
    }
}

int fs_closedir(fs_dir_t *d)
{
    dir_priv_t *dp = dir_of(d);
    int rc;

    if (dp == NULL) {
        return FS_ERR_BADF;
    }
    rc = from_fr(f_closedir(&dp->dir), dp->vol);
    dp->magic = 0;
    return rc;
}

int fs_read_file(const char *path, void *buf, size_t max, size_t *out_len)
{
    fs_file_t f;
    size_t got = 0;
    int64_t size;
    int rc;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (buf == NULL && max != 0) {
        return FS_ERR_INVAL;
    }
    rc = fs_open(&f, path, FS_READ);
    if (rc != FS_OK) {
        return rc;
    }
    size = fs_size(&f);
    while (got < max) {
        rc = fs_read(&f, (unsigned char *)buf + got, max - got);
        if (rc <= 0) {
            break;
        }
        got += (size_t)rc;
    }
    fs_close(&f);
    if (out_len != NULL) {
        *out_len = got;
    }
    if (rc < 0) {
        return rc;
    }
    return (uint64_t)size > max ? FS_ERR_TOOBIG : FS_OK;
}

#if !FF_FS_READONLY
/* "DIR/NAME.EXT" to "DIR/NAME.TMP". out holds FS_PATH_MAX + 1 bytes. */
static int tmp_name(const char *path, char *out)
{
    size_t len, i, base, dot;

    len = k_strlen(path);
    base = 0;
    for (i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':') {
            base = i + 1;
        }
    }
    if (base == len) {
        return FS_ERR_NAME;
    }
    dot = len;
    for (i = base; i < len; i++) {
        if (path[i] == '.') {
            dot = i;
        }
    }
    if (dot + 4 > FS_PATH_MAX) {
        return FS_ERR_NAMETOOLONG;
    }
    if (dot + 4 == len && (path[dot + 1] | 0x20) == 't' && (path[dot + 2] | 0x20) == 'm' &&
        (path[dot + 3] | 0x20) == 'p') {
        return FS_ERR_NAME;
    }
    k_memcpy(out, path, dot);
    k_memcpy(out + dot, ".TMP", 5);
    return FS_OK;
}
#endif

int fs_write_file_atomic(const char *path, const void *buf, size_t len)
{
#if FF_FS_READONLY
    (void)path;
    (void)buf;
    (void)len;
    return FS_ERR_RO;
#else
    char tmp[FS_PATH_MAX + 1];
    fs_file_t f;
    size_t done = 0;
    int rc;

    if (path == NULL || (buf == NULL && len != 0)) {
        return FS_ERR_INVAL;
    }
    if (k_strlen(path) > FS_PATH_MAX) {
        return FS_ERR_NAMETOOLONG;
    }
    rc = tmp_name(path, tmp);
    if (rc != FS_OK) {
        return rc;
    }
    rc = fs_open(&f, tmp, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (rc != FS_OK) {
        return rc;
    }
    while (done < len) {
        rc = fs_write(&f, (const unsigned char *)buf + done, len - done);
        if (rc <= 0) {
            break;
        }
        done += (size_t)rc;
        rc = FS_OK;
    }
    if (rc == FS_OK) {
        rc = fs_sync(&f);
    }
    if (rc != FS_OK) {
        fs_close(&f);
        fs_unlink(tmp);
        return rc < 0 ? rc : FS_ERR_NOSPC;
    }
    rc = fs_close(&f);
    if (rc != FS_OK) {
        return rc;
    }
    /* FAT cannot rename over a file: remove the old one first. */
    rc = fs_unlink(path);
    if (rc != FS_OK && rc != FS_ERR_NOENT) {
        return rc;
    }
    return fs_rename(tmp, path);
#endif
}

int fs_mkfs(blk_dev_t *dev, const fs_mkfs_opts_t *opts)
{
#if FF_FS_READONLY || !FF_USE_MKFS
    (void)dev;
    (void)opts;
    return FS_ERR_RO;
#else
    static const BYTE fmts[] = { FM_FAT | FM_FAT32, FM_FAT, FM_FAT32 };
    BYTE work[FF_MAX_SS] __attribute__((aligned(8)));
    MKFS_PARM parm;
    char drv[3];
    int i, slot = -1, rc;

    if (dev == NULL || dev->block_size != BLK_SECTOR_SIZE ||
        (opts != NULL && opts->fmt >= sizeof(fmts))) {
        return FS_ERR_INVAL;
    }
    rc = check_dev_free(dev);
    if (rc != FS_OK) {
        return rc;
    }
    for (i = 0; i < (int)FF_VOLUMES; i++) {
        if (fs_vols[i].dev == NULL && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return FS_ERR_BUSY;
    }
    k_memset(&parm, 0, sizeof(parm));
    parm.fmt = (BYTE)(fmts[opts != NULL ? opts->fmt : 0] | FM_SFD);
    parm.n_fat = 2;
    parm.au_size = opts != NULL ? opts->cluster_bytes : 0;
    fs_vols[slot].dev = dev;
    fs_vols[slot].blk_err = 0;
    drive_prefix(drv, slot);
    rc = from_fr(f_mkfs(drv, &parm, work, sizeof(work)), slot);
    if (rc == FS_OK) {
        rc = from_fr(disk_ioctl((BYTE)slot, CTRL_SYNC, NULL) == RES_OK ? FR_OK : FR_DISK_ERR, slot);
    }
    fs_vols[slot].dev = NULL;
    return rc;
#endif
}
