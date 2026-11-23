#ifndef ROBCO_TFT_H
#define ROBCO_TFT_H

#include <stdint.h>

/* Channel order of a 32-bit source pixel. The top byte is ignored. */
#define TFT_SRC_XRGB        0u      /* 0x00RRGGBB, the framebuffer's order */
#define TFT_SRC_XBGR        1u      /* 0x00BBGGRR */

/*
 * RGB565 from 8-bit channels. Each channel keeps its top bits: 5 for red
 * and blue, 6 for green. The low bits are dropped, not rounded.
 */
uint16_t tft_rgb565(unsigned r, unsigned g, unsigned b);

/* tft_rgb565() of a 32-bit pixel in TFT_SRC_XRGB or TFT_SRC_XBGR order. */
uint16_t tft_px32_to_565(uint32_t px, unsigned order);

#endif
