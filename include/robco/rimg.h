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
#ifndef ROBCO_IMG_ABI_VERSION
#define ROBCO_IMG_ABI_VERSION 1
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
#endif

#endif
