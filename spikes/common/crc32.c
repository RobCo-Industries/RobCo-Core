#include "crc32.h"

uint32_t crc32_iso_hdlc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}
