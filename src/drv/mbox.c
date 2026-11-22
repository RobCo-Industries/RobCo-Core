#include <stdint.h>

#include <robco/board.h>
#include <robco/cache.h>
#include <robco/mbox.h>
#include <robco/mmio.h>

#define MBOX_OFFSET     0xb880
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20
#define MBOX_FULL       (1u << 31)
#define MBOX_EMPTY      (1u << 30)
#define MBOX_CHANNEL    8

#define MBOX_REQUEST    0x00000000
#define MBOX_RESPONSE   0x80000000
#define MBOX_TIMEOUT    1000000

/*
 * Weak: images without src/arch/cache.S never turn the D-cache on, so the
 * buffer needs no maintenance there. With the cache module linked, these
 * calls do nothing while SCTLR.C is clear.
 */
#pragma weak cache_flush_range
#pragma weak cache_invalidate_range

/*
 * The GPU reads and writes the buffer in memory, past the ARM caches.
 * Clean it before the request, so the GPU sees it. Invalidate it after
 * the answer, so the CPU does not read lines it fetched before or during
 * the GPU write. cache_invalidate_range() cleans lines only partly inside
 * the buffer instead of dropping them, so neighbouring data survives.
 */
int mbox_call(uint32_t *buf)
{
    const robco_board_t *b = board_get();
    uint32_t base;
    uint32_t msg;
    uint32_t size;
    uint32_t i;

    if (b->periph_base == 0 || ((uintptr_t)buf & 0xf) != 0) {
        return -1;
    }
    base = b->periph_base + MBOX_OFFSET;
    size = buf[0];
    if (cache_flush_range) {
        cache_flush_range(buf, size);
    }

    /* The VC sees ARM memory through the bus alias. */
    msg = (((uint32_t)(uintptr_t)buf) | b->bus_alias) & ~0xfu;
    msg |= MBOX_CHANNEL;

    mmio_dsb();
    for (i = 0; mmio_read(base + MBOX_STATUS) & MBOX_FULL; i++) {
        if (i > MBOX_TIMEOUT) {
            return -1;
        }
    }
    mmio_write(base + MBOX_WRITE, msg);

    for (i = 0;; i++) {
        if (i > MBOX_TIMEOUT) {
            return -1;
        }
        if (mmio_read(base + MBOX_STATUS) & MBOX_EMPTY) {
            continue;
        }
        /* Answers for other channels are dropped. */
        if (mmio_read(base + MBOX_READ) == msg) {
            break;
        }
    }
    mmio_dmb();
    if (cache_invalidate_range) {
        cache_invalidate_range(buf, size);
    }

    return ((volatile uint32_t *)buf)[1] == MBOX_RESPONSE ? 0 : -1;
}

/*
 * One cache line of its own: no other data shares it during a call.
 * mbox_call() is not reentrant, so one buffer serves every helper.
 */
static uint32_t tag_buf[16] __attribute__((aligned(CACHE_LINE_MAX)));

/* Single-tag request with two value words. Returns the buffer, values in [5..6], or NULL. */
static __attribute__((noinline)) volatile uint32_t *mbox_tag(uint32_t tag, uint32_t arg)
{
    uint32_t *buf = tag_buf;

    buf[0] = 8 * 4;
    buf[1] = MBOX_REQUEST;
    buf[2] = tag;
    buf[3] = 8;
    buf[4] = 0;
    buf[5] = arg;
    buf[6] = 0;
    buf[7] = 0;
    if (mbox_call(buf) != 0 || (((volatile uint32_t *)buf)[4] & MBOX_RESPONSE) == 0) {
        return 0;
    }
    return buf;
}

int mbox_get_board_rev(uint32_t *rev)
{
    volatile uint32_t *buf = mbox_tag(MBOX_TAG_BOARD_REV, 0);

    if (!buf) {
        return -1;
    }
    *rev = buf[5];
    return 0;
}

int mbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    volatile uint32_t *buf = mbox_tag(MBOX_TAG_ARM_MEMORY, 0);

    if (!buf) {
        return -1;
    }
    *base = buf[5];
    *size = buf[6];
    return 0;
}

int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz)
{
    volatile uint32_t *buf = mbox_tag(MBOX_TAG_CLOCK_RATE, clock_id);

    if (!buf || buf[5] != clock_id || buf[6] == 0) {
        return -1;
    }
    *hz = buf[6];
    return 0;
}
