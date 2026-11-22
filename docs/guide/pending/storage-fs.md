# Storage and files

These sections cover `blk.h`, `blk_ram.h`, `part.h` and `fs.h`. They follow the format of [API.md](../API.md) and go after its Heap section.

Contents rows:

| Header | Section |
|---|---|
| `blk.h` | [Block devices](#block-devices-blkh) |
| `blk_ram.h` | [RAM disk](#ram-disk-blk_ramh) |
| `part.h` | [Partitions](#partitions-parth) |
| `fs.h` | [FAT file system](#fat-file-system-fsh) |

## Block devices (`blk.h`)

`blk.h` is the interface between storage drivers and the file system. A driver fills in a `blk_dev_t`; the file system and `part.h` only call the checked wrappers below. Blocks are 512 bytes.

```c
typedef struct blk_dev blk_dev_t;

struct blk_dev {
    const char *name;
    uint32_t block_size;
    uint64_t block_count;
    uint32_t flags;
    int (*read)(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*sync)(blk_dev_t *dev);
    void *priv;
};
```

`block_count` is `0` when unknown. `sync` may be NULL. `priv` belongs to the driver.

| Constant | Value | Meaning |
|---|---|---|
| `BLK_SECTOR_SIZE` | `512u` | Bytes per block |
| `BLK_FLAG_READONLY` | `0x1u` | Writes fail with `BLK_ERR_RO` |
| `BLK_FLAG_REMOVABLE` | `0x2u` | The medium can go away |
| `BLK_ERR_IO` | `-1` | The device reported an error |
| `BLK_ERR_TIMEOUT` | `-2` | The device did not answer in time |
| `BLK_ERR_RANGE` | `-3` | Block number or count out of range |
| `BLK_ERR_NOMEDIA` | `-4` | No card or medium present |
| `BLK_ERR_RO` | `-5` | The device is read-only |
| `BLK_ERR_PARAM` | `-6` | Bad argument |

```c
static inline int blk_check(const blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);
static inline int blk_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf);
static inline int blk_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);
static inline int blk_sync(blk_dev_t *dev);
static inline const char *blk_strerror(int err);
```

`blk_check()` returns `BLK_ERR_PARAM` for a NULL device or buffer or a zero count, and `BLK_ERR_RANGE` when the blocks run past `block_count`. `blk_read()` and `blk_write()` run that check, then call the driver. `blk_write()` also returns `BLK_ERR_RO` on a read-only device. `blk_sync()` returns `0` when the driver has no `sync`. `blk_strerror()` gives a short name for a code.

## RAM disk (`blk_ram.h`)

`blk_ram.h` turns a buffer into a block device. Tests use it, and so can a RAM disk. The code lives in `src/lib/` and runs on the host.

```c
int blk_ram_init(blk_dev_t *dev, void *buf, size_t size, uint32_t flags);
```

Fills `dev` with a device over `size` bytes at `buf`, rounded down to whole blocks. `flags` takes `BLK_FLAG_*`. The buffer must outlive the device. Returns `BLK_ERR_PARAM` if `dev` or `buf` is NULL or `size` is under one block.

## Partitions (`part.h`)

`part.h` reads an MBR partition table and gives a block device limited to one partition. Errors are `BLK_ERR_*` codes. The code lives in `src/lib/` and runs on the host.

```c
typedef struct {
    blk_dev_t *dev;
    uint8_t index;
    uint8_t type;
    uint8_t bootable;
    uint8_t superfloppy;
    uint64_t start_lba;
    uint64_t block_count;
} part_info_t;
```

`index` is the MBR slot, `0` to `3`. `type` is the MBR type byte. `bootable` is `1` when the active flag is set. `superfloppy` is `1` for a FAT volume that starts at block 0 with no table.

| Constant | Value | Meaning |
|---|---|---|
| `PART_MAX` | `4u` | Primary entries in an MBR |
| `PART_TYPE_FAT12` | `0x01u` | FAT12 |
| `PART_TYPE_FAT16_SM` | `0x04u` | FAT16 under 32 MiB |
| `PART_TYPE_EXTENDED` | `0x05u` | Extended partition |
| `PART_TYPE_FAT16` | `0x06u` | FAT16 |
| `PART_TYPE_FAT32` | `0x0Bu` | FAT32, CHS |
| `PART_TYPE_FAT32_LBA` | `0x0Cu` | FAT32, LBA |
| `PART_TYPE_FAT16_LBA` | `0x0Eu` | FAT16, LBA |
| `PART_TYPE_EXT_LBA` | `0x0Fu` | Extended partition, LBA |
| `PART_TYPE_GPT` | `0xEEu` | Protective MBR of a GPT disk |

```c
int part_scan(blk_dev_t *dev, part_info_t *out, unsigned max);
```

Reads block 0. With an MBR, stores up to `max` used primary entries in slot order and returns how many it stored. It skips empty entries and entries that do not fit the device. It lists extended partitions but does not follow them. With a FAT boot sector at block 0, it stores one superfloppy entry covering the whole device and returns `1`. Returns `0` when block 0 holds neither, or the device error.

```c
int part_open(blk_dev_t *dev, const part_info_t *p, blk_dev_t *out);
```

Fills `out` with a device whose block 0 is `p->start_lba` on `dev`. `out` rejects blocks past `p->block_count` with `BLK_ERR_RANGE` and copies the flags of `dev`. `out` points at `p`, so `p` must stay valid while `out` is in use. `p->dev` must be `dev`. Returns `BLK_ERR_RANGE` when the partition does not fit the device.

```c
part_info_t parts[PART_MAX];
blk_dev_t vol;

if (part_scan(sd, parts, PART_MAX) > 0 && part_open(sd, &parts[0], &vol) == 0) {
    fs_mount("sd", &vol);
}
```

## FAT file system (`fs.h`)

`fs.h` reads and writes FAT12, FAT16 and FAT32 volumes on any `blk_dev_t`. It wraps ChaN's FatFs R0.16, which lives unchanged in `third_party/fatfs/`. Projects include only `fs.h`. The configuration is `src/fs/ffconf.h`. The code has no register access and runs on the host in the unit tests.

| Topic | Rule |
|---|---|
| Paths | `"sd:/DIR/FILE.EXT"`. Without `vol:`, a path uses the default volume: the first one mounted, or the last one passed to `fs_chdir()`. `/` and `\` both separate. `.` and `..` work. |
| Names | Long names, UTF-8, at most 255 bytes. Lookups ignore case. Every long name also gets an 8.3 alias. Boot files use plain 8.3 names such as `/ROBCO/MFBOOT.SYS`. |
| Volumes | Two at a time. |
| Memory | No heap. Each mounted volume uses a 512-byte sector buffer in a static table. Long names use one static 512-byte buffer. `fs_mkfs()` uses 512 bytes of stack. |
| Locking | None. Call from one context at a time. |
| Open files | Do not open the same file twice for writing, and do not unlink or rename an open file. FatFs does not check. |
| Time | Every timestamp is 2026-01-01 00:00 until there is a clock. |
| Read-only build | `#define ROBCO_FS_READONLY 1` in `robco_config.h` drops all write code. Functions that would write return `FS_ERR_RO`. |
| Size | On the target, reading adds about 11.7 KiB of code; read and write about 21.7 KiB. Both add about 2.2 KiB of BSS. An image that calls no `fs_` function adds nothing. |

Every function returns `FS_OK` (`0`) or a count on success, or a negative code:

| Constant | Value | Meaning |
|---|---|---|
| `FS_ERR_IO` | `-1` | The device reported an error |
| `FS_ERR_TIMEOUT` | `-2` | The device did not answer in time |
| `FS_ERR_NOTREADY` | `-3` | No medium, or the device is not ready |
| `FS_ERR_RO` | `-4` | Read-only device or build |
| `FS_ERR_INVAL` | `-5` | Bad argument or flags |
| `FS_ERR_NOENT` | `-6` | No such file or directory |
| `FS_ERR_EXIST` | `-7` | The name already exists |
| `FS_ERR_DENIED` | `-8` | Not allowed: directory not empty, root directory full, wrong open mode, read-only file |
| `FS_ERR_ISDIR` | `-9` | A directory where a file was expected |
| `FS_ERR_NAME` | `-10` | Invalid name |
| `FS_ERR_NAMETOOLONG` | `-11` | Path longer than `FS_PATH_MAX` |
| `FS_ERR_NOSPC` | `-12` | Volume full |
| `FS_ERR_NOVOL` | `-13` | No such volume, or not mounted |
| `FS_ERR_NOFS` | `-14` | No FAT file system on the device |
| `FS_ERR_BUSY` | `-15` | No free volume slot, name in use, or device mounted |
| `FS_ERR_BADF` | `-16` | Handle not open |
| `FS_ERR_CORRUPT` | `-17` | Inconsistent file system |
| `FS_ERR_NOMEM` | `-18` | A work buffer was too small |
| `FS_ERR_MKFS` | `-19` | The device cannot hold the requested FAT type |
| `FS_ERR_TOOBIG` | `-20` | The file is larger than the buffer |

Device errors map as follows: `BLK_ERR_IO` to `FS_ERR_IO`, `BLK_ERR_TIMEOUT` to `FS_ERR_TIMEOUT`, `BLK_ERR_NOMEDIA` to `FS_ERR_NOTREADY`, `BLK_ERR_RO` to `FS_ERR_RO`, `BLK_ERR_PARAM` to `FS_ERR_INVAL` and `BLK_ERR_RANGE` to `FS_ERR_CORRUPT`.

| Constant | Value | Meaning |
|---|---|---|
| `FS_VOLUMES` | `2u` | Volumes mounted at the same time |
| `FS_VOL_MAX` | `7u` | Longest volume name |
| `FS_PATH_MAX` | `255u` | Longest path, volume prefix included |
| `FS_NAME_MAX` | `255u` | Longest name in bytes |

```c
const char *fs_strerror(int err);
```

Short message for an `FS_ERR_*` code. Unknown codes give `"unknown error"`.

### Volumes

```c
int fs_mount(const char *volume, blk_dev_t *dev);
int fs_unmount(const char *volume);
```

`fs_mount()` reads the boot sector of `dev` and makes it available as `volume`. A name is 1 to 7 characters without `:`, `/` or `\`. `dev` must have 512-byte blocks: pass a partition from `part_open()`, or the whole device for a superfloppy. Returns `FS_ERR_NOFS` when there is no FAT volume, `FS_ERR_BUSY` when the name is in use or both slots are taken, or the device error. A read-only device mounts, and every write then returns `FS_ERR_RO`.

`fs_unmount()` forgets the volume and calls `blk_sync()`. It does not flush open files: close them first. Handles still open on the volume return `FS_ERR_BADF` afterwards, even after a new mount.

```c
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t cluster_bytes;
    uint8_t fat_type;
} fs_statvfs_t;

int fs_statvfs(const char *volume, fs_statvfs_t *out);
```

`volume` is a name such as `"sd"`. `fat_type` is `12`, `16` or `32`. The first call on a large FAT32 volume may scan the whole FAT.

```c
typedef struct {
    unsigned fmt;
    uint32_t cluster_bytes;
} fs_mkfs_opts_t;

int fs_mkfs(blk_dev_t *dev, const fs_mkfs_opts_t *opts);
```

Writes a new empty FAT volume over all of `dev`, with no partition table, two FATs and the volume label `NO NAME`. `dev` must not be mounted, and one volume slot must be free. NULL `opts` means `FS_MKFS_ANY` and a cluster size picked from the device size. Returns `FS_ERR_MKFS` when the device is too small or too large for the requested type, and `FS_ERR_RO` on a read-only device or build.

| Constant | Value | Meaning |
|---|---|---|
| `FS_MKFS_ANY` | `0u` | FAT12 or FAT16 by size, FAT32 for large devices |
| `FS_MKFS_FAT` | `1u` | FAT12 or FAT16 only |
| `FS_MKFS_FAT32` | `2u` | FAT32 only; needs about 32 MiB or more |

```c
int fs_chdir(const char *path);
```

Sets the current directory of the path's volume and makes that volume the default. Relative paths start there.

### Files

```c
typedef struct { union { uint64_t align; unsigned char raw[FS_FILE_SIZE]; } priv; } fs_file_t;

int fs_open(fs_file_t *f, const char *path, unsigned flags);
int fs_close(fs_file_t *f);
```

The caller owns `fs_file_t`; its contents are private. `fs_open()` needs `FS_READ`, `FS_WRITE` or both. `FS_CREATE`, `FS_TRUNC` and `FS_APPEND` need `FS_WRITE`; `FS_EXCL` needs `FS_CREATE`. Returns `FS_ERR_INVAL` for other combinations, `FS_ERR_NOENT` for a missing file or directory on the way, `FS_ERR_ISDIR` for a directory, `FS_ERR_EXIST` with `FS_EXCL`, and `FS_ERR_RO` for a write mode on a read-only volume. A new file reaches the disk on `fs_sync()` or `fs_close()`.

`fs_close()` writes back what is buffered and frees the handle. A second close returns `FS_ERR_BADF`.

| Flag | Value | Meaning |
|---|---|---|
| `FS_READ` | `0x01u` | Allow reads |
| `FS_WRITE` | `0x02u` | Allow writes |
| `FS_CREATE` | `0x04u` | Create the file if missing |
| `FS_TRUNC` | `0x08u` | Cut the file to 0 bytes |
| `FS_APPEND` | `0x10u` | Every write goes to the end |
| `FS_EXCL` | `0x20u` | With `FS_CREATE`, fail if the file exists |

```c
int fs_read(fs_file_t *f, void *buf, size_t len);
int fs_write(fs_file_t *f, const void *buf, size_t len);
```

Both return the bytes transferred or an error. A count above `0x7FFFFFFF` is clipped. `fs_read()` returns `0` at the end of the file. When the volume fills up, `fs_write()` returns the bytes that fit, then `FS_ERR_NOSPC` on the next call. A handle opened without the needed mode returns `FS_ERR_DENIED`. After a device error the handle keeps returning that error; close it.

```c
int fs_seek(fs_file_t *f, int64_t offset, int whence);
int64_t fs_tell(fs_file_t *f);
int64_t fs_size(fs_file_t *f);
int fs_sync(fs_file_t *f);
```

`whence` is `FS_SEEK_SET` (`0`), `FS_SEEK_CUR` (`1`) or `FS_SEEK_END` (`2`). A target below `0` or above 4 GiB - 1 returns `FS_ERR_INVAL`. Seeking past the end of a handle opened with `FS_WRITE` extends the file with zeros at once, so the gap reads back as zeros and uses space. Without `FS_WRITE` the position stops at the end of the file. `fs_sync()` writes the file's data and directory entry, then syncs the device.

### Names and directories

```c
typedef struct {
    uint32_t size;
    uint8_t attr;
    uint8_t is_dir;
    uint16_t date, time;
    char name[FS_NAME_MAX + 1];
    char short_name[13];
} fs_stat_t;

int fs_stat(const char *path, fs_stat_t *out);
```

`attr` holds `FS_ATTR_RDONLY` (`0x01`), `FS_ATTR_HIDDEN` (`0x02`), `FS_ATTR_SYSTEM` (`0x04`), `FS_ATTR_DIR` (`0x10`) and `FS_ATTR_ARCHIVE` (`0x20`). `date` and `time` use the FAT format. `name` is spelled as in `path`; `fs_readdir()` gives the stored spelling. The root directory gives `is_dir` set and the name `/`.

```c
int fs_unlink(const char *path);
int fs_rename(const char *from, const char *to);
int fs_mkdir(const char *path);
```

`fs_unlink()` removes a file or an empty directory; a non-empty directory gives `FS_ERR_DENIED`. `fs_rename()` moves a file or directory within one volume; it returns `FS_ERR_EXIST` when `to` exists and `FS_ERR_INVAL` across volumes. `fs_mkdir()` creates one directory; the parent must exist.

```c
typedef struct { union { uint64_t align; unsigned char raw[FS_DIR_SIZE]; } priv; } fs_dir_t;

int fs_opendir(fs_dir_t *d, const char *path);
int fs_readdir(fs_dir_t *d, fs_stat_t *out);
int fs_closedir(fs_dir_t *d);
```

`fs_readdir()` returns `1` with an entry in `out`, `0` at the end, or an error. Entries come in directory order, which is creation order on a fresh volume. `.` and `..` are skipped. From `tests/host/fs_tool.c`:

```c
while ((rc = fs_readdir(&d, &st)) == 1) {
    if (st.is_dir) {
        printf("d %s\n", st.name);
    } else {
        printf("f %lu %s\n", (unsigned long)st.size, st.name);
    }
}
```

### Whole files

```c
int fs_read_file(const char *path, void *buf, size_t max, size_t *out_len);
```

Reads a file into `buf`. Stores the bytes read in `*out_len` when `out_len` is not NULL. Returns `FS_ERR_TOOBIG` when the file is larger than `max`; `buf` then holds the first `max` bytes.

```c
int fs_write_file_atomic(const char *path, const void *buf, size_t len);
```

Replaces a file so that a power cut leaves either the old or the new content. It writes `buf` to a temp file named after the target with the extension `.TMP` (`SAVE1.SAV` uses `SAVE1.TMP`), syncs it, removes the target and renames the temp file over it. Two targets that differ only in extension share one temp name. A target that ends in `.TMP` returns `FS_ERR_NAME`.

Guarantee: if the call fails or the power goes before the temp file is complete, the target keeps its old content. A stale temp file is replaced by the next call. Limit: FAT cannot rename over an existing file. Between the removal and the rename the target is missing and only the `.TMP` file holds the data, which is complete at that point. A reader that finds the target missing and the `.TMP` file present can use the `.TMP` file only if the target existed before; on a first write, a `.TMP` without a target may be partial. The guarantee also needs a device that does not reorder or lose a write it has acknowledged after `blk_sync()`.

```c
uint32_t fs_time_hook(void);
```

Timestamp source in FAT format, date in the high 16 bits. The weak default returns 2026-01-01 00:00. A clock driver can override it. FatFs does not call it while `src/fs/ffconf.h` sets `FF_FS_NORTC` to `1`.
