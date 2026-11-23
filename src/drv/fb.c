#include <stddef.h>
#include <stdint.h>

#include <robco/cache.h>
#include <robco/fb.h>
#include <robco/klibc.h>
#include <robco/mbox.h>
#include <robco/mmu.h>

/* Weak: images that never use the MMU do not link it. */
#pragma weak mmu_map

#define MBOX_RESPONSE       0x80000000u

#define TAG_ALLOCATE        0x00040001
#define TAG_GET_PIXEL_ORDER 0x00040006
#define TAG_GET_PITCH       0x00040008
#define TAG_SET_PHYS_WH     0x00048003
#define TAG_SET_VIRT_WH     0x00048004
#define TAG_SET_DEPTH       0x00048005
#define TAG_SET_PIXEL_ORDER 0x00048006

/* 36 words used, padded to whole 64-byte lines. Fits ROBCO_MBOX_BOUNCE_SIZE. */
#define FB_MSG_WORDS        48

/* mbox_call() does the cache maintenance, whatever the alignment. */
static uint32_t fb_msg[FB_MSG_WORDS] __attribute__((aligned(CACHE_LINE_MAX)));
static fb_info_t fb_state;
static int fb_ready;

/* Appends a tag with `words` value words. Returns the index of value word 0. */
static uint32_t put_tag(uint32_t *n, uint32_t tag, uint32_t words, uint32_t v0, uint32_t v1)
{
    uint32_t at;

    fb_msg[(*n)++] = tag;
    fb_msg[(*n)++] = words * 4;
    fb_msg[(*n)++] = 0;
    at = *n;
    fb_msg[(*n)++] = v0;
    if (words > 1) {
        fb_msg[(*n)++] = v1;
    }
    return at;
}

static int tag_ok(uint32_t value_at)
{
    return (((volatile uint32_t *)fb_msg)[value_at - 1] & MBOX_RESPONSE) != 0;
}

int fb_init(uint32_t width, uint32_t height, uint32_t depth)
{
    volatile uint32_t *m = fb_msg;
    fb_info_t info;
    uint32_t n = 2;
    uint32_t phys, virt, dep, ord, alloc, pitch, ord_rb;

    if (depth != 32) {
        return FB_ERR_DEPTH;
    }
    if (width == 0 || height == 0) {
        return FB_ERR_MISMATCH;
    }

    phys = put_tag(&n, TAG_SET_PHYS_WH, 2, width, height);
    virt = put_tag(&n, TAG_SET_VIRT_WH, 2, width, height);
    dep = put_tag(&n, TAG_SET_DEPTH, 1, depth, 0);
    ord = put_tag(&n, TAG_SET_PIXEL_ORDER, 1, FB_ORDER_RGB, 0);
    alloc = put_tag(&n, TAG_ALLOCATE, 2, 16, 0);
    pitch = put_tag(&n, TAG_GET_PITCH, 1, 0, 0);
    ord_rb = put_tag(&n, TAG_GET_PIXEL_ORDER, 1, 0, 0);
    fb_msg[n++] = 0;
    fb_msg[0] = n * 4;
    fb_msg[1] = 0;

    if (mbox_call(fb_msg) != 0) {
        return FB_ERR_MBOX;
    }
    if (!tag_ok(phys) || !tag_ok(virt) || !tag_ok(dep) || !tag_ok(ord) ||
        !tag_ok(alloc) || !tag_ok(pitch) || !tag_ok(ord_rb)) {
        return FB_ERR_MBOX;
    }
    if (m[phys] != width || m[phys + 1] != height ||
        m[virt] != width || m[virt + 1] != height || m[dep] != depth) {
        return FB_ERR_MISMATCH;
    }

    info.addr = (uint8_t *)(uintptr_t)(m[alloc] & 0x3fffffffu);
    info.size = m[alloc + 1];
    info.width = width;
    info.height = height;
    info.depth = depth;
    info.pitch = m[pitch];
    info.order = m[ord_rb];
    if (info.addr == NULL || info.pitch < width * 4 || info.size < info.pitch * height) {
        return FB_ERR_BUFFER;
    }

    /*
     * The buffer lies in GPU memory, outside the RAM map. Map it
     * non-cacheable so CPU writes reach the GPU without a flush. Covers
     * fb_init() after mmu_enable(); mmu_init() covers the other order.
     */
    if (mmu_map) {
        mmu_map((uint32_t)(uintptr_t)info.addr, info.size, MMU_ATTR_FB);
    }

    fb_state = info;
    fb_ready = 1;
    return 0;
}

int fb_adopt(const fb_info_t *info)
{
    uintptr_t a;

    if (info == NULL || info->addr == NULL) {
        return FB_ERR_ADDR;
    }
    a = (uintptr_t)info->addr;
    if ((a & 3u) != 0 || (info->size != 0 && a + (info->size - 1u) < a)) {
        return FB_ERR_ADDR;
    }
    if (info->width == 0 || info->height == 0) {
        return FB_ERR_SIZE;
    }
    if (info->depth != 32) {
        return FB_ERR_DEPTH;
    }
    /* Divisions, not products: a huge width or height must not overflow. */
    if ((info->pitch & 3u) != 0 || info->pitch / 4u < info->width ||
        info->size / info->pitch < info->height) {
        return FB_ERR_BUFFER;
    }
    if (info->order != FB_ORDER_BGR && info->order != FB_ORDER_RGB) {
        return FB_ERR_ORDER;
    }

    /* The same mapping as fb_init(): the buffer is in GPU memory. */
    if (mmu_map) {
        mmu_map((uint32_t)a, info->size, MMU_ATTR_FB);
    }

    fb_state = *info;
    fb_ready = 1;
    return 0;
}

const fb_info_t *fb_get(void)
{
    return fb_ready ? &fb_state : NULL;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* RGB order puts red in the low byte of the little-endian word. */
    if (fb_state.order == FB_ORDER_RGB) {
        return 0xff000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t *row_ptr(uint32_t y)
{
    return (uint32_t *)(void *)(fb_state.addr + y * fb_state.pitch);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t colour)
{
    if (!fb_ready || x >= fb_state.width || y >= fb_state.height) {
        return;
    }
    row_ptr(y)[x] = colour;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour)
{
    uint32_t *row;
    uint32_t i, j;

    if (!fb_ready || x >= fb_state.width || y >= fb_state.height) {
        return;
    }
    if (w > fb_state.width - x) {
        w = fb_state.width - x;
    }
    if (h > fb_state.height - y) {
        h = fb_state.height - y;
    }
    for (j = 0; j < h; j++) {
        row = row_ptr(y + j) + x;
        for (i = 0; i < w; i++) {
            row[i] = colour;
        }
    }
}

void fb_blit_1bpp(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const uint8_t *bits, uint32_t fg, uint32_t bg)
{
    uint32_t stride = (w + 7) / 8;
    uint32_t cw = w, ch = h;
    const uint8_t *src;
    uint32_t *row;
    uint32_t i, j;

    if (!fb_ready || x >= fb_state.width || y >= fb_state.height) {
        return;
    }
    if (cw > fb_state.width - x) {
        cw = fb_state.width - x;
    }
    if (ch > fb_state.height - y) {
        ch = fb_state.height - y;
    }
    for (j = 0; j < ch; j++) {
        src = bits + j * stride;
        row = row_ptr(y + j) + x;
        for (i = 0; i < cw; i++) {
            row[i] = (src[i >> 3] & (0x80u >> (i & 7))) ? fg : bg;
        }
    }
}

void fb_scroll_up(uint32_t pixels, uint32_t bg)
{
    uint32_t h = fb_state.height;
    uint32_t y;

    if (!fb_ready) {
        return;
    }
    if (pixels >= h) {
        fb_fill_rect(0, 0, fb_state.width, h, bg);
        return;
    }
    if (pixels == 0) {
        return;
    }
    for (y = 0; y + pixels < h; y++) {
        k_memcpy(row_ptr(y), row_ptr(y + pixels), fb_state.width * 4);
    }
    fb_fill_rect(0, h - pixels, fb_state.width, pixels, bg);
}

void fb_clear(uint32_t colour)
{
    if (fb_ready) {
        fb_fill_rect(0, 0, fb_state.width, fb_state.height, colour);
    }
}
