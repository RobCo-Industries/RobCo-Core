/* RGB565 conversion: truncation of each channel, both source orders. */
#include <stdint.h>

#include <robco/tft.h>

#include "rc_test.h"

typedef struct {
    uint32_t px;        /* 0x00RRGGBB */
    uint16_t want;
} row_t;

/* Truncation: 0x07 red stays 0, 0x08 is the first step, 0xff keeps 0x1f. */
static const row_t table[] = {
    { 0x000000u, 0x0000u },     /* black */
    { 0xffffffu, 0xffffu },     /* white */
    { 0xff0000u, 0xf800u },     /* red */
    { 0x00ff00u, 0x07e0u },     /* green */
    { 0x0000ffu, 0x001fu },     /* blue */
    { 0x070307u, 0x0000u },     /* below one step on every channel */
    { 0x080408u, 0x0821u },     /* one step on every channel */
    { 0x0f070fu, 0x0821u },     /* just below two steps: no rounding up */
    { 0x7f7f7fu, 0x7befu },     /* mid grey */
    { 0x808080u, 0x8410u },
    { 0x123456u, 0x11aau },
    { 0xff000000u, 0x0000u },   /* the top byte is ignored */
};

TEST(rgb565_channels)
{
    CHECK_EQ(tft_rgb565(0, 0, 0), 0x0000);
    CHECK_EQ(tft_rgb565(255, 255, 255), 0xffff);
    CHECK_EQ(tft_rgb565(255, 0, 0), 0xf800);
    CHECK_EQ(tft_rgb565(0, 255, 0), 0x07e0);
    CHECK_EQ(tft_rgb565(0, 0, 255), 0x001f);
    CHECK_EQ(tft_rgb565(7, 3, 7), 0x0000);
    CHECK_EQ(tft_rgb565(8, 4, 8), 0x0821);
}

TEST(xrgb_table)
{
    unsigned i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        CHECK_EQ(tft_px32_to_565(table[i].px, TFT_SRC_XRGB), table[i].want);
    }
}

/* The same table with red and blue swapped in the source. */
TEST(xbgr_table)
{
    unsigned i;
    uint32_t p;
    uint32_t s;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        p = table[i].px;
        s = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
        CHECK_EQ(tft_px32_to_565(s, TFT_SRC_XBGR), table[i].want);
    }
    CHECK_EQ(tft_px32_to_565(0x0000ffu, TFT_SRC_XBGR), 0xf800);
    CHECK_EQ(tft_px32_to_565(0xff0000u, TFT_SRC_XBGR), 0x001f);
}
