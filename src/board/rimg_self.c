#include <robco/rimg.h>

/* The header start.S emits at offset 0x20 of the running image. */
extern const rimg_header_t robco_rimg;
extern const uint8_t __image_start[];

int rimg_self_check(void)
{
    const rimg_header_t *h = &robco_rimg;

    if (h->file_size < RIMG_HDR_OFFSET + RIMG_HDR_SIZE || h->crc32 != rimg_crc(__image_start, h->file_size)) {
        return RIMG_E_CRC;
    }
    return RIMG_OK;
}
