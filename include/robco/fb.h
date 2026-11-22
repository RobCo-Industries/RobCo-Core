#ifndef ROBCO_FB_H
#define ROBCO_FB_H

#include <stdint.h>

#define FB_ORDER_BGR 0
#define FB_ORDER_RGB 1

#define FB_ERR_MBOX     (-1)    /* mailbox call or a tag failed */
#define FB_ERR_DEPTH    (-2)    /* depth other than 32 asked for */
#define FB_ERR_MISMATCH (-3)    /* firmware gave another size or depth */
#define FB_ERR_BUFFER   (-4)    /* no buffer, or pitch or size too small */

typedef struct {
    uint8_t *addr;
    uint32_t size, width, height, pitch, depth, order;
} fb_info_t;

/*
 * Allocates a framebuffer with one property message. Only depth 32.
 * Returns 0, or a negative FB_ERR_* and leaves the previous state alone.
 * After a second successful call the console is stale: call console_init()
 * again to size its grid for the new mode.
 */
int fb_init(uint32_t width, uint32_t height, uint32_t depth);

/* NULL until fb_init succeeds. */
const fb_info_t *fb_get(void);

/* Pixel word for the active pixel order. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* All drawing clips to the screen and does nothing before fb_init. */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t colour);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour);
/* Rows are MSB-left, each padded to a whole byte. */
void fb_blit_1bpp(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const uint8_t *bits, uint32_t fg, uint32_t bg);
/* Moves the screen up and fills the freed rows with `bg`. */
void fb_scroll_up(uint32_t pixels, uint32_t bg);
void fb_clear(uint32_t colour);

#endif
