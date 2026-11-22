#include <stdint.h>

#include <robco/board.h>
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
 * The data cache is off in this slice, so the buffer needs no cache
 * maintenance. Once caches are on, clean before and invalidate after.
 */
int mbox_call(uint32_t *buf)
{
    const robco_board_t *b = board_get();
    uint32_t base;
    uint32_t msg;
    uint32_t i;

    if (b->periph_base == 0 || ((uintptr_t)buf & 0xf) != 0) {
        return -1;
    }
    base = b->periph_base + MBOX_OFFSET;

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

    return ((volatile uint32_t *)buf)[1] == MBOX_RESPONSE ? 0 : -1;
}

/* Single-tag request with two value words. Values land in buf[5..6]. */
static int mbox_tag(uint32_t *buf, uint32_t tag, uint32_t arg)
{
    buf[0] = 8 * 4;
    buf[1] = MBOX_REQUEST;
    buf[2] = tag;
    buf[3] = 8;
    buf[4] = 0;
    buf[5] = arg;
    buf[6] = 0;
    buf[7] = 0;
    if (mbox_call(buf) != 0) {
        return -1;
    }
    if ((((volatile uint32_t *)buf)[4] & MBOX_RESPONSE) == 0) {
        return -1;
    }
    return 0;
}

int mbox_get_board_rev(uint32_t *rev)
{
    uint32_t buf[8] __attribute__((aligned(16)));

    if (mbox_tag(buf, MBOX_TAG_BOARD_REV, 0) != 0) {
        return -1;
    }
    *rev = buf[5];
    return 0;
}

int mbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    uint32_t buf[8] __attribute__((aligned(16)));

    if (mbox_tag(buf, MBOX_TAG_ARM_MEMORY, 0) != 0) {
        return -1;
    }
    *base = buf[5];
    *size = buf[6];
    return 0;
}

int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz)
{
    uint32_t buf[8] __attribute__((aligned(16)));

    if (mbox_tag(buf, MBOX_TAG_CLOCK_RATE, clock_id) != 0) {
        return -1;
    }
    if (buf[5] != clock_id || buf[6] == 0) {
        return -1;
    }
    *hz = buf[6];
    return 0;
}
