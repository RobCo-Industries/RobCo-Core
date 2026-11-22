#ifndef SPIKES_FB_H
#define SPIKES_FB_H

#include <stdint.h>

#define FB_ORDER_BGR 0
#define FB_ORDER_RGB 1

struct fb_info {
    uint32_t raw_addr;   /* as returned by the firmware (bus address) */
    uint32_t addr;       /* raw_addr & 0x3fffffff, ARM physical */
    uint32_t size;
    uint32_t pitch;      /* bytes per row */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t order;      /* FB_ORDER_*, read back from the firmware */
    uint32_t mbox_alias; /* bus alias OR-ed into the mailbox buffer address */
};

/*
 * Allocates a framebuffer with one property-channel message:
 * physical and virtual size, depth, pixel order RGB, allocate, pitch,
 * then read back the pixel order. Returns 0 on success.
 * Call with the D-cache off: the buffer is not cleaned or invalidated.
 */
int fb_alloc(uintptr_t periph_base, uint32_t bus_alias, uint32_t width,
             uint32_t height, uint32_t depth, struct fb_info *out);

/* 32-bit pixel word for 8-bit components, in the given order, alpha 0xff. */
static inline uint32_t fb_rgb(uint32_t order, uint32_t r, uint32_t g, uint32_t b)
{
    if (order == FB_ORDER_RGB) {
        return 0xff000000u | (b << 16) | (g << 8) | r;
    }
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

#endif
