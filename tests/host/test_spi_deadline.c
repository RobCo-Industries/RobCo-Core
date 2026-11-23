/*
 * The deadline rule of src/drv/spi0.c against a fake SPI0 controller and
 * a fake clock. Every register read costs 1 us of fake time. The fake
 * moves one byte from the TX FIFO to the RX FIFO every `byte_us`, which
 * plays a slow host under QEMU, and can stop moving after some bytes,
 * which plays a dead bus.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/mbox.h>
#include <robco/timer.h>

#define FAKE_BASE   0x3f000000u
#define SPI_CS_REG  (FAKE_BASE + 0x204000u)
#define SPI_FIFO    (FAKE_BASE + 0x204004u)
#define SPI_CLK     (FAKE_BASE + 0x204008u)
#define FIFO_DEPTH  64u

static uint64_t now;
static uint64_t byte_us;        /* fake time per byte on the wire */
static uint64_t next_move;      /* when the next byte reaches RX */
static uint64_t stall_after;    /* bytes that move before the bus dies */
static uint64_t moved;
static unsigned txq;
static unsigned rxq;
static unsigned overflow;

static void advance(void)
{
    now++;
    while (txq > 0 && moved < stall_after && now >= next_move) {
        txq--;
        rxq++;
        moved++;
        next_move += byte_us;
        if (rxq > FIFO_DEPTH) {
            overflow++;
        }
    }
}

static uint32_t fake_read(uint32_t addr)
{
    advance();
    if (addr == SPI_CS_REG) {
        return (txq < FIFO_DEPTH ? 1u << 18 : 0) | (rxq > 0 ? 1u << 17 : 0) |
               (txq == 0 ? 1u << 16 : 0);
    }
    if (addr == SPI_FIFO && rxq > 0) {
        rxq--;
    }
    return 0;
}

static void fake_write(uint32_t addr, uint32_t v)
{
    if (addr == SPI_CS_REG && (v & (3u << 4))) {
        txq = rxq = 0;
    } else if (addr == SPI_FIFO) {
        if (txq == 0) {
            next_move = now + byte_us;
        }
        txq++;
    }
}

/* mmio.h holds ARM barriers: keep it out and route the driver here. */
#define ROBCO_MMIO_H
#define mmio_read(a) fake_read(a)
#define mmio_write(a, v) fake_write((a), (v))

#include "../../src/drv/spi0.c"

#include "rc_test.h"

static robco_board_t board = { .periph_base = FAKE_BASE };

const robco_board_t *board_get(void) { return &board; }
int gpio_set_function(unsigned pin, unsigned fn)
{
    (void)pin;
    (void)fn;
    return 0;
}
int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz)
{
    (void)clock_id;
    *hz = 250000000u;
    return 0;
}
uint64_t timer_now_us(void) { return now; }
int timer_expired(uint64_t deadline_us) { return now >= deadline_us; }

#define FRAME       (320u * 240u * 2u)  /* one RGB565 display frame */
#define HZ_LOW      3815u               /* 250 MHz / 65534 */
#define HZ_HIGH     125000000u          /* 250 MHz / 2 */

static uint8_t buf[FRAME];

static void setup(uint32_t hz, uint64_t per_byte, uint64_t stall)
{
    CHECK_EQ(spi_init(), 0);
    CHECK_EQ(spi_configure(0, hz, SPI_MODE_0), 0);
    now = 0;
    byte_us = per_byte;
    stall_after = stall;
    moved = 0;
    overflow = 0;
}

/* Window the driver promises: 20 ms plus four times one byte's wire time. */
static uint64_t window(void)
{
    return 20000u + 32000000ull / spi_get_hz(0);
}

/* 2 ms per byte: a full frame takes 307 s of fake time, far past any fixed bound. */
static void slow_frame(uint32_t hz)
{
    setup(hz, 2000, UINT64_MAX);
    CHECK_EQ(spi_write(0, buf, FRAME), 0);
    CHECK_EQ(moved, FRAME);
    CHECK(now > 300000000ull);
    CHECK_EQ(overflow, 0);
}

TEST(slow_frame_completes_low_rate) { slow_frame(HZ_LOW); }
TEST(slow_frame_completes_high_rate) { slow_frame(HZ_HIGH); }

/* A gap just under the window between two bytes is still progress. */
TEST(gap_just_under_window_completes)
{
    setup(HZ_HIGH, 0, UINT64_MAX);
    byte_us = window() - 100;
    CHECK_EQ(spi_write(0, buf, 64), 0);
    CHECK_EQ(moved, 64);
}

static void stalled(uint32_t hz, uint64_t after)
{
    uint64_t t_last;

    setup(hz, 50, after);
    CHECK_EQ(spi_write(0, buf, FRAME), SPI_ERR_TIMEOUT);
    CHECK_EQ(moved, after);
    t_last = after * 50;
    /* Fails after a full window without progress, and not much later. */
    CHECK(now >= t_last + window());
    CHECK(now <= t_last + window() + 1000);
    /* A timeout releases the bus. */
    CHECK_EQ(spi_begin(1), 0);
    CHECK_EQ(spi_end(1), 0);
}

TEST(stall_midway_fails_low_rate) { stalled(HZ_LOW, FRAME / 2); }
TEST(stall_midway_fails_high_rate) { stalled(HZ_HIGH, FRAME / 2); }
TEST(dead_from_start_fails_low_rate) { stalled(HZ_LOW, 0); }
TEST(dead_from_start_fails_high_rate) { stalled(HZ_HIGH, 0); }

TEST(window_values)
{
    setup(HZ_LOW, 0, UINT64_MAX);
    CHECK_EQ(spi_get_hz(0), 3814u);
    CHECK_EQ(stall_us(0), 20000u + 8390u);  /* 32 bits at 3814 Hz */
    setup(HZ_HIGH, 0, UINT64_MAX);
    CHECK_EQ(stall_us(0), 20000u);
}
