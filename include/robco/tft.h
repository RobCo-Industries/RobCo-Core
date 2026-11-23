#ifndef ROBCO_TFT_H
#define ROBCO_TFT_H

#include <stddef.h>
#include <stdint.h>

/*
 * ILI9341 TFT panel on SPI0, 16 bits per pixel (RGB565), 4-wire SPI with
 * a data/command GPIO. Polled, main context only. The glass is 240x320;
 * rotation 90 and 270 use it as 320x240. Not validated on hardware.
 */

#define TFT_ERR_ARG         (-1)    /* bad config field, rotation, order or NULL buffer */
#define TFT_ERR_INIT        (-2)    /* tft_init() has not run or failed */
#define TFT_ERR_BOUNDS      (-3)    /* the rectangle is empty or not inside the panel */
#define TFT_ERR_BUS         (-4)    /* an SPI or GPIO call failed */
#define TFT_ERR_TIMER       (-5)    /* the system timer does not run */
#define TFT_ERR_NO_PANEL    (-6)    /* the id read gave all zeros or all ones */

#define TFT_PIN_NONE        (-1)

#define TFT_DEFAULT_HZ      32000000u
#define TFT_HZ_MIN          1000000u
#define TFT_HZ_MAX          64000000u

/* Rate used by tft_read_id(). The data sheet's read cycle is slower than its write cycle. */
#define TFT_READ_HZ         4000000u

/* Pixels converted per SPI write: a 128-byte stack buffer. */
#define TFT_CHUNK_PIXELS    64u

/* Channel order of a 32-bit source pixel. The top byte is ignored. */
#define TFT_SRC_XRGB        0u      /* 0x00RRGGBB, the framebuffer's order */
#define TFT_SRC_XBGR        1u      /* 0x00BBGGRR */

typedef struct {
    unsigned cs;            /* SPI0 chip select, 0 or 1 */
    uint32_t hz;            /* SPI clock asked for, TFT_HZ_MIN to TFT_HZ_MAX */
    int dc_pin;             /* data/command GPIO: low for a command byte */
    int reset_pin;          /* reset GPIO, active low, or TFT_PIN_NONE */
    int backlight_pin;      /* backlight GPIO, high is on, or TFT_PIN_NONE */
    unsigned rotation;      /* 0, 90, 180 or 270 degrees */
    int bgr;                /* non-zero sets the BGR bit of MADCTL */
    int invert;             /* non-zero sends INVON, zero sends INVOFF */
} tft_config_t;

/* The default wiring of docs/spec/BOARD_SUPPORT.md: CE0, DC on GPIO 25, reset on GPIO 4. */
#define TFT_CONFIG_DEFAULT { 0u, TFT_DEFAULT_HZ, 25, 4, TFT_PIN_NONE, 90u, 1, 0 }

/*
 * Checks the config, runs spi_init() if no rate is set yet, sets the
 * rate, resets the panel and sends the init sequence. Returns 0 or a
 * negative TFT_ERR_*. Returns TFT_ERR_TIMER without touching a pin when
 * the system timer does not advance.
 */
int tft_init(const tft_config_t *cfg);

/* Panel size for the rotation set, or 0 before tft_init(). */
unsigned tft_width(void);
unsigned tft_height(void);

/* SPI rate actually set by tft_init(), or 0 before it. */
uint32_t tft_get_hz(void);

/*
 * Sends a `w` by `h` rectangle of RGB565 pixels, row by row, `w` pixels
 * per row with no padding. The rectangle must lie inside the panel:
 * nothing is clipped and nothing is sent on TFT_ERR_BOUNDS.
 */
int tft_write_rect(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *pixels);

/* Fills a rectangle with one RGB565 colour. Same bounds rule. */
int tft_fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t colour);

/*
 * Sends a whole frame from a 32-bit buffer of tft_width() by tft_height()
 * pixels, rows `pitch_bytes` apart, converting to RGB565 on the way.
 * `order` is TFT_SRC_XRGB or TFT_SRC_XBGR.
 */
int tft_present_px32(const uint32_t *src, uint32_t pitch_bytes, unsigned order);

/*
 * Sends only the rectangle x, y, w, h of the same full-frame buffer:
 * `src` still points at pixel (0, 0).
 */
int tft_present_px32_rect(const uint32_t *src, uint32_t pitch_bytes, unsigned order,
                          unsigned x, unsigned y, unsigned w, unsigned h);

/* Sleep in (non-zero) or out, then waits 120 ms. The backlight follows. */
int tft_sleep(int on);

/* Display on or off. The picture in panel memory is kept. The backlight follows. */
int tft_display(int on);

/*
 * Sends RDDID (0x04) at TFT_READ_HZ and stores the four bytes read after
 * it, first byte in the top bits. Needs MISO wired. Returns
 * TFT_ERR_NO_PANEL when they are all zeros or all ones.
 */
int tft_read_id(uint32_t *id);

/*
 * Optional observer of every byte written to the panel, with the DC
 * level: 0 for a command byte, 1 for data. NULL turns it off. Used by
 * tests and examples to checksum the bus.
 */
typedef void (*tft_tap_fn)(int dc, const uint8_t *bytes, size_t len);
void tft_set_tap(tft_tap_fn fn);

/*
 * RGB565 from 8-bit channels. Each channel keeps its top bits: 5 for red
 * and blue, 6 for green. The low bits are dropped, not rounded.
 */
uint16_t tft_rgb565(unsigned r, unsigned g, unsigned b);

/* tft_rgb565() of a 32-bit pixel in TFT_SRC_XRGB or TFT_SRC_XBGR order. */
uint16_t tft_px32_to_565(uint32_t px, unsigned order);

#endif
