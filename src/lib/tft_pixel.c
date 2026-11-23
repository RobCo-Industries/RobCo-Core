#include <stdint.h>

#include <robco/tft.h>

/* Keeps the top bits of each channel: truncation, no rounding. */
uint16_t tft_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | ((b & 0xffu) >> 3));
}

uint16_t tft_px32_to_565(uint32_t px, unsigned order)
{
    unsigned r = (px >> 16) & 0xffu;
    unsigned g = (px >> 8) & 0xffu;
    unsigned b = px & 0xffu;

    if (order == TFT_SRC_XBGR) {
        return tft_rgb565(b, g, r);
    }
    return tft_rgb565(r, g, b);
}
