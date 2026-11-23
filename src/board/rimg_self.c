#include <robco/rimg.h>

/* The header start.S emits at offset 0x20 of the running image. */
extern const rimg_header_t robco_rimg;
extern const uint8_t __image_start[];
/* Linker symbol: its address is the file size, see ld/image.ld.S. */
extern const uint8_t __image_file_size[];

int rimg_self_check(void)
{
    uint32_t n;
    int rc;

    /* Trust no header field before it matches the linked value. */
    rc = rimg_self_scan(&robco_rimg, ROBCO_IMG_MAGIC, (uint32_t)(uintptr_t)__image_start,
                        (uint32_t)(uintptr_t)__image_file_size, &n);
    if (rc != RIMG_OK) {
        return rc;
    }
    if (robco_rimg.crc32 != rimg_crc(__image_start, n)) {
        return RIMG_E_CRC;
    }
    return RIMG_OK;
}
