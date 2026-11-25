#ifndef ROBCO_RIMG_H
#define ROBCO_RIMG_H

/*
 * rimg image layout. See docs/spec/IMAGE_FORMAT.md.
 *
 *   0x00  branch to the entry point, padded to 0x20
 *   0x20  rimg_header_t, 64 bytes
 *   0x60  rimg_meta_t, 192 bytes, only if flags bit 0 is set
 *   then  code and data
 */

#define RIMG_HDR_OFFSET     0x20
#define RIMG_META_OFFSET    0x60
#define RIMG_HDR_VERSION    1
#define RIMG_HDR_SIZE       64
#define RIMG_META_SIZE      192
#define RIMG_FLAG_META      (1 << 0)

/* Magics, 8 bytes each, NUL-padded. */
#define RIMG_MAGIC_BOOT     "BOOT\0\0\0\0"
#define RIMG_MAGIC_ROM1     "ROM1\0\0\0\0"
#define RIMG_MAGIC_ROBCO78  "ROBCO78\0"

/* BOOT subtypes. 1 is reserved. */
#define RIMG_BOOT_UOS       0
#define RIMG_BOOT_MAINT     2
#define RIMG_BOOT_DIAG      3
#define RIMG_BOOT_AGENT     4
#define RIMG_BOOT_BIOS      5
#define RIMG_BOOT_PIPOS     6
#define RIMG_BOOT_EXAMPLE   7

/* ROM1 subtypes. */
#define RIMG_ROM1_SYSTEM    0

/* ROBCO78 subtypes. */
#define RIMG_HOLO_GAME      0
#define RIMG_HOLO_UTILITY   1
#define RIMG_HOLO_DATA      2

/*
 * Header defaults for the image start.S builds.
 * The project's robco_config.h may override them.
 */
#ifndef ROBCO_IMG_MAGIC
#define ROBCO_IMG_MAGIC RIMG_MAGIC_BOOT
#endif
#ifndef ROBCO_IMG_SUBTYPE
#define ROBCO_IMG_SUBTYPE RIMG_BOOT_EXAMPLE
#endif
/* (major << 16) | minor. BOOT images use major 2, the boot info version. */
#ifndef ROBCO_IMG_ABI_VERSION
#define ROBCO_IMG_ABI_VERSION (2 << 16)
#endif
#ifndef ROBCO_IMG_VERSION
#define ROBCO_IMG_VERSION 0
#endif
#ifndef ROBCO_IMG_NAME
#define ROBCO_IMG_NAME "robco"
#endif
#ifndef ROBCO_IMG_FLAGS
#define ROBCO_IMG_FLAGS 0
#endif

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef struct __attribute__((packed)) rimg_header {
    char     magic[8];
    uint16_t hdr_version;
    uint16_t hdr_size;
    uint32_t flags;
    uint32_t load_addr;
    uint32_t file_size;     /* size of the raw image */
    uint32_t mem_size;      /* file size + bss + stacks */
    uint32_t crc32;         /* 0 until a host tool seals the image */
    uint32_t abi_version;
    uint32_t image_version;
    uint32_t subtype;
    char     name[16];
    uint32_t reserved;
} rimg_header_t;

typedef struct __attribute__((packed)) rimg_meta {
    char    title[64];
    uint8_t icon[128];
} rimg_meta_t;

_Static_assert(sizeof(rimg_header_t) == RIMG_HDR_SIZE, "rimg_header_t must be 64 bytes");
_Static_assert(sizeof(rimg_meta_t) == RIMG_META_SIZE, "rimg_meta_t must be 192 bytes");

#include <stddef.h>

typedef enum rimg_err {
    RIMG_OK        = 0,
    RIMG_E_NOFILE  = 1,
    RIMG_E_IO      = 2,
    RIMG_E_MAGIC   = 3,
    RIMG_E_VERSION = 4,
    RIMG_E_SIZE    = 5,
    RIMG_E_BOUNDS  = 6,
    RIMG_E_ABI     = 7,
    RIMG_E_CRC     = 8,
} rimg_err_t;

/*
 * What the loader accepts.
 *   magic      8 bytes, such as RIMG_MAGIC_BOOT. NULL accepts the three known magics.
 *   slot_base  Slot the image must fit. slot_size 0 uses the slot of the subtype.
 *   abi_major  ABI major. 0 uses the major of the magic: 2 for BOOT, 1 otherwise.
 *   file_size  Size from the FAT directory entry. 0 skips that comparison.
 */
typedef struct rimg_expect {
    const char *magic;
    uint32_t slot_base;
    uint32_t slot_size;
    uint32_t abi_major;
    uint32_t file_size;
} rimg_expect_t;

/*
 * Runs validation steps 1 to 6 from IMAGE_FORMAT.md on an image in memory.
 * `avail` is the number of readable bytes at `image`. The image's file_size
 * must not exceed it. Fewer than 0x60 bytes gives RIMG_E_IO.
 * Returns RIMG_OK and sets *out (if not NULL) to the header, or the code of
 * the first failed step. Touches no hardware.
 */
int rimg_validate(const void *image, size_t avail, const rimg_expect_t *expect,
                  const rimg_header_t **out);

/*
 * Runs steps 1 to 5 on the first 0x60 bytes of an image, before the rest
 * is read. `head` holds at least 0x60 bytes. `avail` is the size of the
 * whole file. Fewer than 0x60 bytes gives RIMG_E_IO. Reads only `head`.
 */
int rimg_check_header(const void *head, size_t avail, const rimg_expect_t *expect);

/*
 * The image CRC: CRC-32 over file bytes [0, file_size) with the crc32
 * field at offset 0x3C read as zero. Returns 0 when image is NULL or
 * file_size is below 0x40. Touches no hardware.
 */
uint32_t rimg_crc(const void *image, uint32_t file_size);

/*
 * The header checks of rimg_self_check(), before it reads past the header.
 * Compares the running image's header `h` with the values the image was
 * linked with, in the order of the validation steps: magic (RIMG_E_MAGIC),
 * hdr_version and hdr_size (RIMG_E_VERSION), file_size (RIMG_E_SIZE),
 * load_addr (RIMG_E_BOUNDS). On RIMG_OK, *scan is the number of bytes the
 * CRC may cover, which is `file_size`. Touches no hardware.
 */
int rimg_self_scan(const rimg_header_t *h, const char magic[8], uint32_t load_addr,
                   uint32_t file_size, uint32_t *scan);

/*
 * Checks the running image against its own sealed CRC, over the whole
 * file with no skipped range. It first checks the header with
 * rimg_self_scan() against the linked magic, load address and file size,
 * so a corrupt file_size gives RIMG_E_SIZE and no read past the image.
 * Returns RIMG_OK, the code of a failed header check, or RIMG_E_CRC.
 * Call it early: a .data variable the image wrote before the call changes
 * the result. The startup code keeps its own state out of the file bytes.
 */
int rimg_self_check(void);

/* The @RC name of a rimg_err_t code, such as "crc". "unknown" for others. */
const char *rimg_strerror(int code);

/*
 * Sealing and packing, src/lib/rimg_seal.c. See IMAGE_FORMAT.md,
 * "Sealing and packing". They give the same bytes and codes as
 * tools/rimg.py seal and pack. Touches no hardware.
 */

/* Icon file formats, as rimg.py --icon-format. */
typedef enum rimg_icon_format {
    RIMG_ICON_AUTO = 0,     /* 128 bytes is raw, any other length is PBM */
    RIMG_ICON_RAW  = 1,     /* exactly 128 bytes in the META layout */
    RIMG_ICON_PBM  = 2,     /* a 32x32 PBM, P1 or P4 */
} rimg_icon_format_t;

/*
 * Seal inputs. A NULL pointer, or a has_ flag of 0, is an absent input.
 *   name           NUL-terminated text.
 *   image_version  (X << 16) | (Y << 8) | Z, already parsed by the caller.
 *   title          NUL-terminated text.
 *   icon           The icon file's bytes, icon_len of them. The function
 *                  decodes them by icon_format before any step.
 */
typedef struct rimg_seal_opts {
    const char        *name;
    int                has_image_version;
    uint32_t           image_version;
    const char        *title;
    const uint8_t     *icon;
    size_t             icon_len;
    rimg_icon_format_t icon_format;
} rimg_seal_opts_t;

/*
 * Pack inputs. seal holds name, image_version, title and icon. Pack writes
 * image_version into the header at step 7, 0 when absent, and seals with
 * the rest. The numbers are signed and 64 bits wide so that step 2 and
 * step 4 see a value out of range, as rimg.py does.
 *   magic        NUL-terminated: "BOOT", "ROM1" or "ROBCO78".
 *   abi_version  (MAJOR << 16) | MINOR. Absent: the ABI major of the magic, minor 0.
 *   meta         -1 follows the magic, 0 no META block, 1 a META block.
 */
typedef struct rimg_pack_opts {
    rimg_seal_opts_t seal;
    const char      *magic;
    int64_t          subtype;
    int64_t          load_addr;
    int              has_mem_size;
    int64_t          mem_size;
    int              has_abi_version;
    uint32_t         abi_version;
    int              meta;
    int64_t          entry;
} rimg_pack_opts_t;

/*
 * Seals the len bytes at image into out, which holds len bytes and does
 * not overlap image. Runs icon decoding, then seal steps 1 to 12.
 * Returns RIMG_OK, or the code of the first failed step. On a failure the
 * bytes of out are undefined and the caller writes no file.
 * A NULL image, opts or out returns RIMG_E_IO before any step.
 */
int rimg_seal(const void *image, size_t len, const rimg_seal_opts_t *opts, void *out);

/*
 * Packs payload_len bytes into out, which holds out_cap bytes and does not
 * overlap payload. The image is payload_len + 0x60 bytes, or
 * payload_len + 0x120 with a META block. Runs icon decoding, then pack
 * steps 1 to 9. At step 5 a payload too long for a 32-bit file_size gives
 * RIMG_E_SIZE, then an out_cap below the image length gives RIMG_E_IO.
 * Returns RIMG_OK and sets *out_len to the image length, or the code of
 * the first failed check. On a failure *out_len is 0 and the bytes of out
 * are undefined. A NULL opts, out or out_len, a NULL payload with a
 * non-zero payload_len, or a meta other than -1, 0 or 1, returns
 * RIMG_E_IO before any step. *out_len is 0 then too, if out_len is not NULL.
 */
int rimg_pack(const void *payload, size_t payload_len, const rimg_pack_opts_t *opts,
              void *out, size_t out_cap, size_t *out_len);
#endif

#endif
