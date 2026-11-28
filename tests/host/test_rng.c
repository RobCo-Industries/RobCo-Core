/*
 * src/drv/rng.c against a fake generator and a fake clock. Every register
 * read costs 1 us of fake time, and a read of the timer costs none. The
 * fake has no word until `warm_us` after the enable, which plays the
 * warm-up of the real chip, then one more word every `word_us`. The low
 * 20 bits of its status register keep the count the driver wrote, so a
 * driver that took them for words would not wait. QEMU has a word on
 * every status read, so only this test shows the wait.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/timer.h>

#define FAKE_BASE   0x20000000u
#define REG_CTRL    (FAKE_BASE + 0x104000u)
#define REG_STATUS  (FAKE_BASE + 0x104004u)
#define REG_DATA    (FAKE_BASE + 0x104008u)
#define FIFO_DEPTH  4u
#define NEVER       (1ull << 62)

static uint64_t now;            /* fake time in us */
static int timer_stopped;       /* the timer reads one value for ever */
static uint64_t jump_at;        /* a timer read at or after this time... */
static uint64_t jump_us;        /* ...finds this much time gone, once */
static uint32_t reg_ctrl;
static uint32_t warm_count;     /* low 20 bits of the status register, as written */
static uint64_t warm_us;        /* time from the enable to the first word */
static uint64_t word_us;        /* time per word after it */
static uint64_t next_word;      /* when the next word enters the FIFO */
static unsigned fifo;           /* words that wait */
static uint32_t seq;
static unsigned status_reads;
static unsigned data_reads;
static unsigned empty_reads;    /* data reads with no word: the driver makes none */
static unsigned warm_writes;
static unsigned bad_access;

static void fill(void)
{
    while ((reg_ctrl & 1u) && fifo < FIFO_DEPTH && now >= next_word) {
        fifo++;
        next_word += word_us;
    }
}

static uint32_t fake_read(uint32_t addr)
{
    now++;
    fill();
    switch (addr) {
    case REG_CTRL:
        return reg_ctrl;
    case REG_STATUS:
        status_reads++;
        return ((uint32_t)fifo << 24) | warm_count;
    case REG_DATA:
        data_reads++;
        if (fifo == 0) {
            empty_reads++;
            return 0;
        }
        if (fifo == FIFO_DEPTH) {
            /* A full FIFO held the generator: it starts again now. */
            next_word = now + word_us;
        }
        fifo--;
        return ++seq * 0x9e3779b9u;
    default:
        bad_access++;
        return 0;
    }
}

static void fake_write(uint32_t addr, uint32_t v)
{
    if (addr == REG_STATUS) {
        warm_count = v & 0xfffffu;
        warm_writes++;
    } else if (addr == REG_CTRL) {
        if ((v & 1u) && !(reg_ctrl & 1u)) {
            next_word = now + warm_us;
        }
        reg_ctrl = v;
    } else {
        bad_access++;
    }
}

/* mmio.h holds ARM barriers: keep it out and route the driver here. */
#define ROBCO_MMIO_H
#define mmio_read(a) fake_read(a)
#define mmio_write(a, v) fake_write((a), (v))

#include "../../src/drv/rng.c"

#include "rc_test.h"

static robco_board_t board = { .periph_base = FAKE_BASE };

const robco_board_t *board_get(void)
{
    return &board;
}

uint32_t timer_now_lo(void)
{
    if (jump_us != 0 && now >= jump_at) {
        now += jump_us;
        jump_us = 0;
        fill();
    }
    return timer_stopped ? 0x00c0ffeeu : (uint32_t)now;
}

static void fresh(uint64_t start, uint64_t warm, uint64_t per_word)
{
    board.periph_base = FAKE_BASE;
    now = start;
    timer_stopped = 0;
    jump_at = 0;
    jump_us = 0;
    reg_ctrl = 0;
    warm_count = 0;
    warm_us = warm;
    word_us = per_word;
    next_word = 0;
    fifo = 0;
    seq = 0;
    status_reads = 0;
    data_reads = 0;
    empty_reads = 0;
    warm_writes = 0;
    bad_access = 0;
}

TEST(init_arms_the_warm_up_once_and_does_not_wait)
{
    fresh(0, NEVER, 30);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(reg_ctrl & 1u, 1);
    CHECK_EQ(warm_count, 0x40000u);
    CHECK_EQ(warm_writes, 1);
    /* One read of the control register, no status read, no wait. */
    CHECK_EQ(now, 1);
    CHECK_EQ(status_reads, 0);
    /* A generator that runs is left alone: a later stage adds no warm-up. */
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(warm_writes, 1);
    CHECK_EQ(now, 2);
    CHECK_EQ(bad_access, 0);
}

/* A warm-up of 1.5 s, more than a million status reads. The first read waits it out. */
static void warm_up(uint64_t start)
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint64_t t;

    fresh(start, 1500000, 30);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32(&a), 0);
    CHECK(now - start >= 1500000);
    CHECK(now - start <= 1500000 + 10);
    /* The next word comes at the pace of the generator. */
    t = now;
    CHECK_EQ(rng_u32(&b), 0);
    CHECK(now - t <= 40);
    CHECK(a != 0 && b != 0 && a != b);
    CHECK_EQ(data_reads, 2);
    CHECK_EQ(empty_reads, 0);
    CHECK_EQ(bad_access, 0);
}

TEST(a_read_waits_through_the_warm_up) { warm_up(0); }

/* The low word of the timer wraps during the wait. */
TEST(a_read_waits_through_the_warm_up_across_a_timer_wrap) { warm_up(0xfff00000u); }

TEST(a_word_just_inside_the_limit_is_read)
{
    uint32_t w = 0;

    fresh(0, RNG_TIMEOUT_US - 10, 30);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32(&w), 0);
    CHECK(w != 0);
}

TEST(a_generator_with_no_word_times_out_at_the_limit)
{
    uint32_t w = 0x12345678u;
    uint64_t t;

    CHECK_EQ(RNG_TIMEOUT_US, 3000000u);
    fresh(0, NEVER, 30);
    CHECK_EQ(rng_init(), 0);
    t = now;
    CHECK_EQ(rng_u32(&w), RNG_ERR_TIMEOUT);
    CHECK(now - t >= RNG_TIMEOUT_US);
    CHECK(now - t <= RNG_TIMEOUT_US + 10);
    CHECK_EQ(w, 0x12345678u);
    CHECK_EQ(data_reads, 0);
    /* The same without rng_init(): the generator is off. */
    fresh(0, 0, 30);
    CHECK_EQ(rng_u32(&w), RNG_ERR_TIMEOUT);
    CHECK(now >= RNG_TIMEOUT_US);
    CHECK_EQ(reg_ctrl, 0);
    CHECK_EQ(bad_access, 0);
}

/*
 * The wait is held up past the limit, as by a long interrupt, and the
 * word comes in that time. The driver looks once more before it gives up.
 */
TEST(a_wait_held_up_past_the_limit_still_takes_the_word)
{
    uint32_t w = 0;

    fresh(0, 2000000, 30);
    jump_at = 1000000;
    jump_us = 4000000;
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32(&w), 0);
    CHECK(w != 0);
    CHECK(now >= 5000000);
    CHECK_EQ(empty_reads, 0);
}

/* One word every 2 s: each wait is under the limit, the whole call is over it. */
TEST(the_limit_counts_for_each_word)
{
    uint8_t buf[8];

    fresh(0, 2000000, 2000000);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_read(buf, sizeof(buf)), 0);
    CHECK(now >= 4000000);
    CHECK_EQ(data_reads, 2);
}

TEST(a_read_fills_bytes_from_whole_words_low_byte_first)
{
    uint8_t buf[11];
    uint32_t w0 = 1u * 0x9e3779b9u;
    uint32_t w2 = 3u * 0x9e3779b9u;

    memset(buf, 0xaa, sizeof(buf));
    fresh(0, 100, 30);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_read(buf, 10), 0);
    CHECK_EQ(data_reads, 3);
    CHECK_EQ(buf[0], (uint8_t)w0);
    CHECK_EQ(buf[3], (uint8_t)(w0 >> 24));
    CHECK_EQ(buf[8], (uint8_t)w2);
    CHECK_EQ(buf[9], (uint8_t)(w2 >> 8));
    CHECK_EQ(buf[10], 0xaa);
    /* No byte asked: no register read. */
    CHECK_EQ(rng_read(buf, 0), 0);
    CHECK_EQ(data_reads, 3);
    CHECK_EQ(empty_reads, 0);
}

/* A timer that does not advance must not hang the wait. */
TEST(a_stopped_timer_ends_the_wait)
{
    uint32_t w = 0x12345678u;

    fresh(0, NEVER, 30);
    timer_stopped = 1;
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32(&w), RNG_ERR_TIMEOUT);
    CHECK(status_reads <= 1000001);
    CHECK_EQ(w, 0x12345678u);
    /* A word that waits is still read. */
    fresh(0, 0, 30);
    timer_stopped = 1;
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32(&w), 0);
    CHECK(w != 0x12345678u);
}

TEST(no_peripheral_base_is_an_error_and_reads_no_register)
{
    uint32_t w = 0x12345678u;
    uint8_t b[4];

    fresh(0, 0, 30);
    board.periph_base = 0;
    CHECK_EQ(rng_init(), RNG_ERR_BOARD);
    CHECK_EQ(rng_read(b, sizeof(b)), RNG_ERR_BOARD);
    CHECK_EQ(rng_u32(&w), RNG_ERR_BOARD);
    CHECK_EQ(w, 0x12345678u);
    CHECK_EQ(now, 0);
    CHECK_EQ(bad_access, 0);
}

TEST(within_limit_zero_takes_a_word_that_is_ready)
{
    uint32_t w = 0x12345678u;

    fresh(0, 0, 30);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32_within(&w, 0), 0);
    CHECK(w != 0x12345678u);
    CHECK_EQ(status_reads, 1);
    CHECK_EQ(data_reads, 1);
}

/* No word: one status read, then the error, with no wait. */
TEST(within_limit_zero_with_no_word_does_not_wait)
{
    uint32_t w = 0x12345678u;
    uint64_t t;

    fresh(0, NEVER, 30);
    CHECK_EQ(rng_init(), 0);
    t = now;
    CHECK_EQ(rng_u32_within(&w, 0), RNG_ERR_TIMEOUT);
    CHECK_EQ(status_reads, 1);
    CHECK_EQ(now - t, 1);
    CHECK_EQ(w, 0x12345678u);
    CHECK_EQ(data_reads, 0);
}

TEST(within_takes_a_word_that_comes_before_the_limit)
{
    uint32_t w = 0;
    uint64_t t;

    fresh(0, 500, 30);
    CHECK_EQ(rng_init(), 0);
    t = now;
    CHECK_EQ(rng_u32_within(&w, 1000), 0);
    CHECK(w != 0);
    CHECK(now - t <= 510);
}

TEST(within_refuses_a_word_that_comes_after_the_limit)
{
    uint32_t w = 0x12345678u;
    uint64_t t;

    fresh(0, 2000, 30);
    CHECK_EQ(rng_init(), 0);
    t = now;
    CHECK_EQ(rng_u32_within(&w, 1000), RNG_ERR_TIMEOUT);
    CHECK(now - t >= 1000);
    CHECK(now - t <= 1010);
    CHECK_EQ(w, 0x12345678u);
    CHECK_EQ(data_reads, 0);
}

/* The wait is held up past the limit and the word came meanwhile. */
TEST(within_looks_once_more_after_the_limit)
{
    uint32_t w = 0;

    fresh(0, 2000, 30);
    jump_at = 500;
    jump_us = 3000;
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32_within(&w, 1000), 0);
    CHECK(w != 0);
    CHECK_EQ(empty_reads, 0);
}

/*
 * First word at 1 ms, the next 0.6 ms later. Each wait is under 1.5 ms,
 * the whole call is not: the second wait counts the time of the first.
 */
TEST(within_limit_covers_the_whole_call)
{
    uint8_t buf[8];
    uint32_t w0 = 1u * 0x9e3779b9u;

    memset(buf, 0xaa, sizeof(buf));
    fresh(0, 1000, 600);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_read_within(buf, sizeof(buf), 1500), RNG_ERR_TIMEOUT);
    CHECK(now <= 1520);
    CHECK_EQ(data_reads, 1);
    /* The first word is in buf, the rest is unchanged. */
    CHECK_EQ(buf[0], (uint8_t)w0);
    CHECK_EQ(buf[3], (uint8_t)(w0 >> 24));
    CHECK_EQ(buf[4], 0xaa);
    CHECK_EQ(buf[7], 0xaa);
    /* With room for both words the call succeeds. */
    fresh(0, 1000, 600);
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_read_within(buf, sizeof(buf), 1700), 0);
    CHECK_EQ(data_reads, 2);
}

TEST(within_ends_on_a_stopped_timer)
{
    uint32_t w = 0x12345678u;

    fresh(0, NEVER, 30);
    timer_stopped = 1;
    CHECK_EQ(rng_init(), 0);
    CHECK_EQ(rng_u32_within(&w, 1000), RNG_ERR_TIMEOUT);
    CHECK(status_reads <= 1000001);
    CHECK_EQ(w, 0x12345678u);
}

TEST(within_with_no_peripheral_base_is_an_error)
{
    uint32_t w = 0x12345678u;
    uint8_t b[4];

    fresh(0, 0, 30);
    board.periph_base = 0;
    CHECK_EQ(rng_read_within(b, sizeof(b), 1000), RNG_ERR_BOARD);
    CHECK_EQ(rng_u32_within(&w, 1000), RNG_ERR_BOARD);
    CHECK_EQ(w, 0x12345678u);
    CHECK_EQ(now, 0);
}
