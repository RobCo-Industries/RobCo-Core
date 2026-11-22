#include "mbox.h"
#include "mmio.h"

#define MBOX_OFFSET     0xb880
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20
#define MBOX_FULL       (1u << 31)
#define MBOX_EMPTY      (1u << 30)
#define MBOX_CHANNEL    8

#define MBOX_REQUEST    0x00000000
#define MBOX_RESPONSE   0x80000000
#define MBOX_TAG_END    0
#define MBOX_TIMEOUT    1000000

static uintptr_t mbox_base;
static uint32_t mbox_alias;
static volatile uint32_t mbox_buf[16] __attribute__((aligned(16)));

void mbox_init(uintptr_t periph_base, uint32_t bus_alias)
{
    mbox_base = periph_base + MBOX_OFFSET;
    mbox_alias = bus_alias;
}

/*
 * Sends a single-tag request with `value_words` value words.
 * On success the response values are in mbox_buf[5..].
 */
static int mbox_call_tag(uint32_t tag, uint32_t value_words, const uint32_t *in, uint32_t in_words)
{
    uint32_t i;
    uint32_t n;
    uint32_t msg;

    if (mbox_base == 0) {
        return -1;
    }

    n = 0;
    mbox_buf[n++] = 0;
    mbox_buf[n++] = MBOX_REQUEST;
    mbox_buf[n++] = tag;
    mbox_buf[n++] = value_words * 4;
    mbox_buf[n++] = 0;
    for (i = 0; i < value_words; i++) {
        mbox_buf[n++] = i < in_words ? in[i] : 0;
    }
    mbox_buf[n++] = MBOX_TAG_END;
    mbox_buf[0] = n * 4;

    msg = (((uint32_t)(uintptr_t)mbox_buf) | mbox_alias) & ~0xfu;
    msg |= MBOX_CHANNEL;

    dmb();
    for (i = 0; mmio_read(mbox_base + MBOX_STATUS) & MBOX_FULL; i++) {
        if (i > MBOX_TIMEOUT) {
            return -1;
        }
    }
    mmio_write(mbox_base + MBOX_WRITE, msg);

    for (i = 0;; i++) {
        if (i > MBOX_TIMEOUT) {
            return -1;
        }
        if (mmio_read(mbox_base + MBOX_STATUS) & MBOX_EMPTY) {
            continue;
        }
        if (mmio_read(mbox_base + MBOX_READ) == msg) {
            break;
        }
    }
    dmb();

    if (mbox_buf[1] != MBOX_RESPONSE) {
        return -1;
    }
    if ((mbox_buf[4] & MBOX_RESPONSE) == 0) {
        return -1;
    }
    return 0;
}

int mbox_get_board_rev(uint32_t *rev)
{
    if (mbox_call_tag(MBOX_TAG_BOARD_REV, 1, 0, 0) != 0) {
        return -1;
    }
    *rev = mbox_buf[5];
    return 0;
}

int mbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    if (mbox_call_tag(MBOX_TAG_ARM_MEMORY, 2, 0, 0) != 0) {
        return -1;
    }
    *base = mbox_buf[5];
    *size = mbox_buf[6];
    return 0;
}

int mbox_get_clock_rate(uint32_t clock_id, uint32_t *rate)
{
    if (mbox_call_tag(MBOX_TAG_CLOCK_RATE, 2, &clock_id, 1) != 0) {
        return -1;
    }
    if (mbox_buf[5] != clock_id || mbox_buf[6] == 0) {
        return -1;
    }
    *rate = mbox_buf[6];
    return 0;
}
