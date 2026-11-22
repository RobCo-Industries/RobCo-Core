#ifndef ROBCO_LOADER_H
#define ROBCO_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include <robco/rimg.h>

/*
 * Stage loader. Reads a rimg file into a memory slot and validates it as
 * docs/spec/IMAGE_FORMAT.md says. Return codes are rimg_err_t values.
 *
 * Order:
 *   0. Open the file and get its size from the file system (nofile, io).
 *   1-5. Read the first 0x60 bytes into a stack buffer and run the header
 *      steps with the FAT size: magic, version, size, bounds, abi.
 *      Nothing is written to the slot before these pass.
 *   6. Copy the header to the slot, read the rest of the file in chunks
 *      straight after it, then check the CRC over what was loaded.
 *
 * Steps 3 and 4 make file_size equal to the FAT size and keep
 * load_addr + mem_size inside the slot, so the loader writes exactly
 * file_size bytes at the slot base and never a byte outside the slot.
 * No allocation.
 */

/* Bytes per read. Reads after the header stay aligned to this in the file. */
#define LOADER_CHUNK 8192u

/* Longest path the result keeps, with the NUL. */
#define LOADER_PATH_MAX 64u

typedef struct {
    uint32_t base; /* slot base address, the load_addr images must carry */
    uint32_t size; /* slot size in bytes */
    void *mem;     /* where the slot is in memory; NULL means (void *)base */
} loader_slot_t;

typedef struct {
    int err;                     /* rimg_err_t of the file loaded last */
    const rimg_header_t *hdr;    /* header inside the slot; NULL unless loaded */
    uint32_t entry;              /* load_addr, the entry point */
    uint32_t file_size;          /* header file_size */
    uint32_t mem_size;           /* header mem_size */
    uint32_t fat_size;           /* size the file system reports */
    uint32_t crc32;              /* header crc32 */
    char path[LOADER_PATH_MAX];  /* path of the file loaded last, cut to fit */
    int fallback;                /* loader_load_fallback: 1 if the backup was tried */
    int primary_err;             /* loader_load_fallback: why the primary failed, or 0 */
} loader_result_t;

/*
 * File access for the pure core. open() returns RIMG_OK and the size,
 * RIMG_E_NOFILE or RIMG_E_IO. read() returns the bytes read, 0 at the
 * end, or a negative value on error. close() may be NULL.
 */
typedef struct {
    int (*open)(void *ctx, const char *path, uint64_t *size);
    int (*read)(void *ctx, void *buf, uint32_t len);
    void (*close)(void *ctx);
    void *ctx;
} loader_io_t;

/*
 * Loads `path` into `slot` through the file system (fs.h). `expect` gives
 * the magic and ABI major; NULL accepts any magic. Its slot and file_size
 * fields are ignored: the loader uses `slot` and the FAT size. `out`, if
 * not NULL, receives the result also on failure. Returns a rimg_err_t.
 */
int loader_load(const char *path, const loader_slot_t *slot, const rimg_expect_t *expect,
                loader_result_t *out);

/*
 * Loads `primary`, or `backup` when the primary is missing or fails any
 * step. out->fallback tells whether the backup was tried and
 * out->primary_err why the primary failed. Returns the code of the file
 * that was tried last: RIMG_OK when one of them loaded. `backup` may be
 * NULL.
 */
int loader_load_fallback(const char *primary, const char *backup, const loader_slot_t *slot,
                         const rimg_expect_t *expect, loader_result_t *out);

/* The same over any file access. Pure: src/lib/loader_core.c. */
int loader_load_io(const loader_io_t *io, const char *path, const loader_slot_t *slot,
                   const rimg_expect_t *expect, loader_result_t *out);
int loader_load_fallback_io(const loader_io_t *io, const char *primary, const char *backup,
                            const loader_slot_t *slot, const rimg_expect_t *expect,
                            loader_result_t *out);

#endif
