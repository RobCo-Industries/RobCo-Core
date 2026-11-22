#include "fb.h"
#include "mmio.h"

/* Same mailbox registers and protocol as mbox.c, with a larger buffer. */
#define MBOX_OFFSET     0xb880
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20
#define MBOX_FULL       (1u << 31)
#define MBOX_EMPTY      (1u << 30)
#define MBOX_CHANNEL    8
#define MBOX_RESPONSE   0x80000000
#define MBOX_TIMEOUT    1000000

#define TAG_ALLOCATE        0x00040001
#define TAG_GET_PIXEL_ORDER 0x00040006
#define TAG_GET_PITCH       0x00040008
#define TAG_SET_PHYS_WH     0x00048003
#define TAG_SET_VIRT_WH     0x00048004
#define TAG_SET_DEPTH       0x00048005
#define TAG_SET_PIXEL_ORDER 0x00048006

static volatile uint32_t fb_msg[36] __attribute__((aligned(16)));

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
    return (fb_msg[value_at - 1] & MBOX_RESPONSE) != 0;
}

int fb_alloc(uintptr_t periph_base, uint32_t bus_alias, uint32_t width,
             uint32_t height, uint32_t depth, struct fb_info *out)
{
    uintptr_t mbox = periph_base + MBOX_OFFSET;
    uint32_t n = 2;
    uint32_t phys, virt, dep, ord, alloc, pitch, ord_rb;
    uint32_t msg;
    uint32_t i;

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

    out->mbox_alias = bus_alias;
    msg = ((((uint32_t)(uintptr_t)fb_msg) | bus_alias) & ~0xfu) | MBOX_CHANNEL;

    dmb();
    for (i = 0; mmio_read(mbox + MBOX_STATUS) & MBOX_FULL; i++) {
        if (i > MBOX_TIMEOUT) {
            return -1;
        }
    }
    mmio_write(mbox + MBOX_WRITE, msg);
    for (i = 0;; i++) {
        if (i > MBOX_TIMEOUT) {
            return -1;
        }
        if (mmio_read(mbox + MBOX_STATUS) & MBOX_EMPTY) {
            continue;
        }
        if (mmio_read(mbox + MBOX_READ) == msg) {
            break;
        }
    }
    dmb();

    if (fb_msg[1] != MBOX_RESPONSE) {
        return -1;
    }
    if (!tag_ok(phys) || !tag_ok(virt) || !tag_ok(dep) || !tag_ok(ord) ||
        !tag_ok(alloc) || !tag_ok(pitch) || !tag_ok(ord_rb)) {
        return -1;
    }

    out->width = fb_msg[phys];
    out->height = fb_msg[phys + 1];
    out->depth = fb_msg[dep];
    out->raw_addr = fb_msg[alloc];
    out->addr = fb_msg[alloc] & 0x3fffffffu;
    out->size = fb_msg[alloc + 1];
    out->pitch = fb_msg[pitch];
    out->order = fb_msg[ord_rb];
    if (out->raw_addr == 0 || out->size == 0 || out->pitch == 0) {
        return -1;
    }
    return 0;
}
