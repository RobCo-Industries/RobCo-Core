/* Stage loader over the file system: the fs.h side of loader_core.c. */
#include <robco/fs.h>
#include <robco/loader.h>

typedef struct {
    fs_file_t f;
} fs_io_t;

static int io_open(void *ctx, const char *path, uint64_t *size)
{
    fs_io_t *io = ctx;
    int64_t n;
    int rc = fs_open(&io->f, path, FS_READ);

    switch (rc) {
    case FS_OK:
        break;
    case FS_ERR_NOENT:
    case FS_ERR_ISDIR:
    case FS_ERR_NAME:
    case FS_ERR_NAMETOOLONG:
    case FS_ERR_DENIED:
        return RIMG_E_NOFILE;
    default:
        return RIMG_E_IO;
    }
    n = fs_size(&io->f);
    if (n < 0) {
        fs_close(&io->f);
        return RIMG_E_IO;
    }
    *size = (uint64_t)n;
    return RIMG_OK;
}

static int io_read(void *ctx, void *buf, uint32_t len)
{
    fs_io_t *io = ctx;

    return fs_read(&io->f, buf, len);
}

static void io_close(void *ctx)
{
    fs_io_t *io = ctx;

    fs_close(&io->f);
}

int loader_load(const char *path, const loader_slot_t *slot, const rimg_expect_t *expect,
                loader_result_t *out)
{
    fs_io_t f;
    const loader_io_t io = { io_open, io_read, io_close, &f };

    return loader_load_io(&io, path, slot, expect, out);
}

int loader_load_fallback(const char *primary, const char *backup, const loader_slot_t *slot,
                         const rimg_expect_t *expect, loader_result_t *out)
{
    fs_io_t f;
    const loader_io_t io = { io_open, io_read, io_close, &f };

    return loader_load_fallback_io(&io, primary, backup, slot, expect, out);
}
