/*
 * Stage loader core. Pure: all file access goes through a loader_io_t.
 * src/fs/loader.c plugs in the file system.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/klibc.h>
#include <robco/loader.h>
#include <robco/rimg.h>

#define HEAD_SIZE (RIMG_HDR_OFFSET + RIMG_HDR_SIZE)

/* Reads exactly len bytes. RIMG_OK or RIMG_E_IO. */
static int read_all(const loader_io_t *io, uint8_t *buf, uint32_t len)
{
    while (len > 0) {
        int n = io->read(io->ctx, buf, len);

        if (n <= 0 || (uint32_t)n > len) {
            return RIMG_E_IO;
        }
        buf += n;
        len -= (uint32_t)n;
    }
    return RIMG_OK;
}

static void set_path(loader_result_t *out, const char *path)
{
    size_t n = path != NULL ? k_strlen(path) : 0;

    if (n >= sizeof(out->path)) {
        n = sizeof(out->path) - 1;
    }
    if (n > 0) {
        memcpy(out->path, path, n);
    }
    out->path[n] = '\0';
}

static void fill_header(loader_result_t *out, const rimg_header_t *h)
{
    out->entry = h->load_addr;
    out->file_size = h->file_size;
    out->mem_size = h->mem_size;
    out->crc32 = h->crc32;
}

/* Steps 1 to 6 on an open file of `size` bytes. */
static int load_open(const loader_io_t *io, uint64_t size, const loader_slot_t *slot,
                     const rimg_expect_t *expect, loader_result_t *out)
{
    union {
        uint32_t align;
        uint8_t b[HEAD_SIZE];
    } head;
    rimg_expect_t exp = { 0 };
    const rimg_header_t *h = (const rimg_header_t *)(head.b + RIMG_HDR_OFFSET);
    uint8_t *mem;
    uint32_t off;
    int rc;

    out->fat_size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
    if (size < HEAD_SIZE) {
        return RIMG_E_IO;
    }
    if (size > 0xFFFFFFFFu) {
        return RIMG_E_SIZE;
    }
    rc = read_all(io, head.b, HEAD_SIZE);
    if (rc != RIMG_OK) {
        return rc;
    }

    if (expect != NULL) {
        exp.magic = expect->magic;
        exp.abi_major = expect->abi_major;
    }
    exp.slot_base = slot->base;
    exp.slot_size = slot->size;
    exp.file_size = (uint32_t)size;

    /* Steps 1 to 5 from the header alone. The slot is still untouched. */
    rc = rimg_check_header(head.b, (size_t)size, &exp);
    fill_header(out, h);
    if (rc != RIMG_OK) {
        return rc;
    }

    /*
     * Step 4 passed: load_addr == slot base and file_size <= mem_size, and
     * load_addr + mem_size <= slot end. So [0, file_size) of the slot is
     * inside it. Reads never ask for more than what is left of file_size.
     */
    mem = slot->mem != NULL ? slot->mem : (uint8_t *)(uintptr_t)slot->base;
    memcpy(mem, head.b, HEAD_SIZE);
    off = HEAD_SIZE;
    while (off < h->file_size) {
        uint32_t len = LOADER_CHUNK - off % LOADER_CHUNK;

        if (len > h->file_size - off) {
            len = h->file_size - off;
        }
        rc = read_all(io, mem + off, len);
        if (rc != RIMG_OK) {
            return rc;
        }
        off += len;
    }

    /* Step 6, with steps 1 to 5 again on the bytes now in the slot. */
    rc = rimg_validate(mem, h->file_size, &exp, &out->hdr);
    if (rc != RIMG_OK) {
        out->hdr = NULL;
    }
    return rc;
}

int loader_load_io(const loader_io_t *io, const char *path, const loader_slot_t *slot,
                   const rimg_expect_t *expect, loader_result_t *out)
{
    loader_result_t local;
    uint64_t size = 0;
    int rc;

    if (out == NULL) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));
    set_path(out, path);
    if (io == NULL || io->open == NULL || io->read == NULL || slot == NULL || slot->size == 0 ||
        (uint64_t)slot->base + slot->size > 0x100000000ull) {
        out->err = RIMG_E_IO;
        return out->err;
    }
    if (path == NULL) {
        out->err = RIMG_E_NOFILE;
        return out->err;
    }
    rc = io->open(io->ctx, path, &size);
    if (rc == RIMG_OK) {
        rc = load_open(io, size, slot, expect, out);
        if (io->close != NULL) {
            io->close(io->ctx);
        }
    } else if (rc != RIMG_E_NOFILE) {
        rc = RIMG_E_IO;
    }
    out->err = rc;
    return rc;
}

int loader_load_fallback_io(const loader_io_t *io, const char *primary, const char *backup,
                            const loader_slot_t *slot, const rimg_expect_t *expect,
                            loader_result_t *out)
{
    loader_result_t local;
    int first;
    int rc;

    if (out == NULL) {
        out = &local;
    }
    first = loader_load_io(io, primary, slot, expect, out);
    if (first == RIMG_OK || backup == NULL) {
        out->primary_err = first;
        return first;
    }
    rc = loader_load_io(io, backup, slot, expect, out);
    out->fallback = 1;
    out->primary_err = first;
    return rc;
}
