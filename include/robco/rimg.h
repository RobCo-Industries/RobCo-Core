#ifndef ROBCO_RIMG_H
#define ROBCO_RIMG_H

/*
 * RobCo image header, 64 bytes at image offset 0x20.
 * The project's robco_config.h may override these defaults.
 */
#ifndef ROBCO_IMG_MAGIC
#define ROBCO_IMG_MAGIC "RCIMG\0\0\0"
#endif
#ifndef ROBCO_IMG_SUBTYPE
#define ROBCO_IMG_SUBTYPE 0
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

#define RIMG_OFFSET         0x20
#define RIMG_HDR_VERSION    1
#define RIMG_HDR_SIZE       64

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef struct {
    char magic[8];
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
    char name[16];
    uint32_t reserved;
} robco_rimg_t;

_Static_assert(sizeof(robco_rimg_t) == RIMG_HDR_SIZE, "rimg header must be 64 bytes");
#endif

#endif
