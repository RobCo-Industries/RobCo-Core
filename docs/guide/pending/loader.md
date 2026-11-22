## Stage loader (`loader.h`)

`loader.h` loads a `rimg` file from the FAT file system into a memory slot and validates it as [IMAGE_FORMAT.md](../../spec/IMAGE_FORMAT.md) says. RETROS-BIOS, MFBootAgent and Pip-OS use it to load the next stage. It allocates nothing and prints nothing: the caller emits the `@RC` markers. Return codes are `rimg_err_t` values, and `rimg_strerror()` gives their `@RC` names.

Init order: mount the card first: `sd_init()`, `part_scan()`, `part_open()`, then `fs_mount()`. A path without a volume uses the first volume mounted.

| Constant | Value | Meaning |
|---|---|---|
| `LOADER_CHUNK` | `8192u` | Bytes per read. Reads after the header stay aligned to this offset in the file. |
| `LOADER_PATH_MAX` | `64u` | Size of `loader_result_t.path`, with the NUL |

```c
typedef struct {
    uint32_t base; /* slot base address, the load_addr images must carry */
    uint32_t size; /* slot size in bytes */
    void *mem;     /* where the slot is in memory; NULL means (void *)base */
} loader_slot_t;
```

`mem` lets host tests point the slot at a buffer. On the target, leave it `NULL`.

```c
typedef struct {
    int err;
    const rimg_header_t *hdr;
    uint32_t entry, file_size, mem_size, fat_size, crc32;
    char path[LOADER_PATH_MAX];
    int fallback;
    int primary_err;
} loader_result_t;
```

`err` is the code of the file loaded last. `hdr` points at the header inside the slot after a successful load, and is `NULL` otherwise. `entry`, `file_size`, `mem_size` and `crc32` come from the header once it has been read. `fat_size` is the size the file system reports. `path` is the path of the file loaded last, cut to fit. `fallback` and `primary_err` are set by `loader_load_fallback()`.

```c
int loader_load(const char *path, const loader_slot_t *slot, const rimg_expect_t *expect,
                loader_result_t *out);
```

Loads `path` into `slot`. `expect` gives the magic and the ABI major, as for `rimg_validate()`; `NULL` accepts any known magic with its default ABI major. Its slot and `file_size` fields are ignored: the loader uses `slot` and the FAT size. `out` may be `NULL`; when it is not, it is filled on failure too. The checks run in this order:

1. The file opens: `RIMG_E_NOFILE` for a missing file, a missing directory, a directory or a bad name. Any other failure, such as an unmounted volume, is `RIMG_E_IO`.
2. The first `0x60` bytes are read into a stack buffer: `RIMG_E_IO` for a read error or a file shorter than that.
3. Validation steps 1 to 5 run on that header with the FAT size, through `rimg_check_header()`: `magic`, `version`, `size`, `bounds`, `abi`. The slot is not written before they pass.
4. The header is copied to the slot base and the rest of the file is read after it in chunks: `RIMG_E_IO` for a read error or a file that ends early.
5. `rimg_validate()` runs on the slot, so step 6 checks the CRC over the bytes loaded: `RIMG_E_CRC`.

Step 3 makes `file_size` equal to the FAT size and keeps `load_addr + mem_size` within the slot, with `load_addr` equal to the slot base. The loader then writes exactly `file_size` bytes from the slot base. It writes no byte outside the slot, also for a file larger than the slot or larger than its header claims. After a CRC failure the slot holds the bad image; the slot belongs to the image, so this is allowed. The loader does not zero BSS.

```c
int loader_load_fallback(const char *primary, const char *backup, const loader_slot_t *slot,
                         const rimg_expect_t *expect, loader_result_t *out);
```

The fallback rule from [BOOT_PROTOCOL.md](../../spec/BOOT_PROTOCOL.md): loads `primary`, and `backup` when the primary is missing or fails any step. `out->fallback` is 1 when the backup was tried. `out->primary_err` holds why the primary failed, or `RIMG_OK`. The return value and `out->err` belong to the file tried last. `backup` may be `NULL`. RETROS-BIOS loading MFBootAgent:

```c
const loader_slot_t slot = { 0x00200000, 0x00200000, NULL };
const rimg_expect_t expect = { .magic = RIMG_MAGIC_BOOT, .abi_major = 2 };
loader_result_t r;
int rc = loader_load_fallback("/ROBCO/MFBOOT.SYS", "/ROBCO/MFBOOT.BAK", &slot, &expect, &r);

if (r.fallback) {
    k_printf("@RC stage=bios ev=error code=%s path=/ROBCO/MFBOOT.SYS\n", rimg_strerror(r.primary_err));
    k_printf("@RC stage=bios ev=fallback path=/ROBCO/MFBOOT.BAK\n");
}
```

`examples/chain1`, built with `-DCHAIN1_SD=1`, does the whole sequence: mount, load with fallback, boot info and handoff.

```c
typedef struct {
    int (*open)(void *ctx, const char *path, uint64_t *size);
    int (*read)(void *ctx, void *buf, uint32_t len);
    void (*close)(void *ctx);
    void *ctx;
} loader_io_t;

int loader_load_io(const loader_io_t *io, const char *path, const loader_slot_t *slot,
                   const rimg_expect_t *expect, loader_result_t *out);
int loader_load_fallback_io(const loader_io_t *io, const char *primary, const char *backup,
                            const loader_slot_t *slot, const rimg_expect_t *expect,
                            loader_result_t *out);
```

The same loader over any file access. `open()` returns `RIMG_OK` and the size, `RIMG_E_NOFILE`, or another value that counts as `RIMG_E_IO`. `read()` returns the bytes read, which may be fewer than asked, `0` at the end, or a negative value. `close()` may be `NULL`. These live in `src/lib/loader_core.c` and touch no hardware; `loader_load()` plugs in `fs.h`.

## Header check (`rimg.h`)

```c
int rimg_check_header(const void *head, size_t avail, const rimg_expect_t *expect);
```

Runs validation steps 1 to 5 on the first `0x60` bytes of an image before the rest is read. `head` holds at least `0x60` bytes. `avail` is the size of the whole file. Reads only `head`. Returns `RIMG_E_IO` for a `NULL` `head` or `avail` below `0x60`. `rimg_validate()` runs the same checks, then the CRC.

## File time stamps (`fs.h`)

```c
typedef int (*fs_clock_fn)(datetime_t *out);
void fs_set_clock(fs_clock_fn get);
uint32_t fs_time_hook(void);
```

New and modified files get the time of the library clock. The weak `fs_time_hook()` asks the clock set with `fs_set_clock()`, or `clock_get()` from `rtc.h` when the image links the RTC driver. The file system holds only a weak reference to `clock_get()`, so an image without the driver links without it. When the clock is not set, or gives a date outside 2000 to 2099, files get 2026-01-01 00:00. `fs_set_clock(NULL)` goes back to the default. An image with a DS1307 calls `i2c_init()` and `rtc_init()`, which sets the clock; nothing else is needed. A program can still replace `fs_time_hook()` with its own.

## Tick rate (`tick.h`)

```c
uint32_t tick_hz(void);
```

The rate `tick_init()` started, in Hz, or `0` when the tick does not run. The real rate can be a little higher when the rate does not divide 1000000.

## SD card images (`tools/mksdimage.sh`)

```
tools/mksdimage.sh <out.img> <size-mib> <dir>
```

Builds an SD card image file from a directory tree with mtools, without root: an MBR with one FAT partition at LBA 2048, FAT16 below 64 MiB and FAT32 from 64 MiB. The size is rounded up to a power of 2, at least 4 MiB, because QEMU needs that. It refuses a device or a directory as the output and a missing input directory. `tools/mkbootdir.sh` assembles the directory. In a project, `make sdimage SDIMAGE_DIR=<dir>` writes `build/sd.img`; `SDIMAGE_MIB` sets the size, 64 by default.

## Storage example (`examples/storage`)

Mounts the first partition of the card as `sd`, then reads commands on the PL011, one per line, and answers each with one marker: `ls`, `cat`, `put`, `atomic`, `rm`, `mv`, `mkdir`, `df`, `ini`, `sync`, `umount` and `mount`. `put` and `atomic` write byte `i` of the file as `((seed + i) * 2654435761 mod 2^32) >> 24`. `tests/target/test_storage.py` drives it.
