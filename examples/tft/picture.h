/*
 * The test picture of examples/tft and the bus checksum. Shared with the
 * host tool tests/host/tft_tool.c, so the target and the host run the
 * same calls and the CRCs can be compared.
 */
#ifndef TFT_PICTURE_H
#define TFT_PICTURE_H

#include <stddef.h>
#include <stdint.h>

#include <robco/crc32.h>
#include <robco/tft.h>

#define BORDER 2u

static uint32_t bus_crc;
static uint32_t bus_bytes;

/* CRC-32 over every byte sent, each fed as two bytes: its DC level, then the byte. */
static void bus_tap(int dc, const uint8_t *bytes, size_t len)
{
    uint8_t pair[2];
    size_t i;

    pair[0] = (uint8_t)dc;
    for (i = 0; i < len; i++) {
        pair[1] = bytes[i];
        bus_crc = crc32_update(bus_crc, pair, 2);
    }
    bus_bytes += (uint32_t)len;
}

/* Initialises the panel with `cfg` and the tap on. Returns tft_init()'s result. */
static int picture_init(const tft_config_t *cfg)
{
    bus_crc = 0;
    bus_bytes = 0;
    tft_set_tap(bus_tap);
    return tft_init(cfg);
}

/* Eight vertical colour bars and a white border. Returns 0 or the first error. */
static int picture_draw(void)
{
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 },
    };
    unsigned w = tft_width();
    unsigned h = tft_height();
    uint16_t white = tft_rgb565(255, 255, 255);
    unsigned i, x0, x1;
    int rc = 0;

    for (i = 0; i < 8 && rc == 0; i++) {
        x0 = w * i / 8u;
        x1 = w * (i + 1u) / 8u;
        rc = tft_fill_rect(x0, 0, x1 - x0, h, tft_rgb565(bars[i][0], bars[i][1], bars[i][2]));
    }
    if (rc == 0) {
        rc = tft_fill_rect(0, 0, w, BORDER, white);
    }
    if (rc == 0) {
        rc = tft_fill_rect(0, h - BORDER, w, BORDER, white);
    }
    if (rc == 0) {
        rc = tft_fill_rect(0, BORDER, BORDER, h - 2u * BORDER, white);
    }
    if (rc == 0) {
        rc = tft_fill_rect(w - BORDER, BORDER, BORDER, h - 2u * BORDER, white);
    }
    return rc;
}

#endif
