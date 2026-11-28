#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/mmio.h>
#include <robco/rng.h>
#include <robco/timer.h>

#define RNG_OFFSET      0x104000
#define RNG_CTRL        0x00
#define RNG_STATUS      0x04
#define RNG_DATA        0x08

#define RNG_ENABLE      1u
#define RNG_WARMUP      0x40000u
/*
 * Timer reads in a row with no change before a wait calls the system
 * timer stopped. A running 1 MHz counter changes every microsecond, so
 * a million equal reads only happen when the counter does not run.
 */
#define RNG_STALL_POLLS 1000000u

static uint32_t rng_base(void)
{
    const robco_board_t *b = board_get();

    return b->periph_base ? b->periph_base + RNG_OFFSET : 0;
}

int rng_init(void)
{
    uint32_t base = rng_base();

    if (base == 0) {
        return RNG_ERR_BOARD;
    }
    if (!(mmio_read(base + RNG_CTRL) & RNG_ENABLE)) {
        /*
         * The generator discards its first output: the warm-up. It gives
         * no word until that is over, so the first read waits. A
         * generator that already runs is left alone.
         */
        mmio_write(base + RNG_STATUS, RNG_WARMUP);
        mmio_write(base + RNG_CTRL, RNG_ENABLE);
    }
    return 0;
}

/*
 * Waits for one word until `limit` microseconds after t0 on the system
 * timer. Returns 0 and stores it, or RNG_ERR_TIMEOUT. The time is the low
 * word of the timer: the difference of two reads stays right when it
 * wraps. After the limit the status is read once more, so a wait that was
 * held up, as by a long interrupt, still takes a word that came in the
 * meantime. A wait that starts past the limit reads the status once. A
 * timer that does not advance ends the wait after RNG_STALL_POLLS reads.
 */
static int rng_word(uint32_t base, uint32_t *out, uint32_t t0, uint32_t limit)
{
    uint32_t last = timer_now_lo();
    uint32_t same = 0;
    uint32_t t;
    int late = last - t0 >= limit;

    while (!(mmio_read(base + RNG_STATUS) >> 24)) {
        if (late) {
            return RNG_ERR_TIMEOUT;
        }
        t = timer_now_lo();
        if (t != last) {
            last = t;
            same = 0;
        } else if (++same >= RNG_STALL_POLLS) {
            return RNG_ERR_TIMEOUT;
        }
        late = t - t0 >= limit;
    }
    *out = mmio_read(base + RNG_DATA);
    return 0;
}

/*
 * Fills buf word by word. With per_word set, each word gets `limit` from
 * the start of its own wait. Else `limit` counts from the start of the
 * call and covers every word.
 */
static int rng_fill(void *buf, size_t len, uint32_t limit, int per_word)
{
    uint32_t base = rng_base();
    unsigned char *p = buf;
    uint32_t t0;
    uint32_t w;
    size_t i;
    int rc;

    if (base == 0) {
        return RNG_ERR_BOARD;
    }
    t0 = timer_now_lo();
    while (len > 0) {
        if (per_word) {
            t0 = timer_now_lo();
        }
        rc = rng_word(base, &w, t0, limit);
        if (rc < 0) {
            return rc;
        }
        for (i = 0; i < 4 && len > 0; i++, len--) {
            *p++ = (unsigned char)(w >> (i * 8));
        }
    }
    return 0;
}

int rng_read(void *buf, size_t len)
{
    return rng_fill(buf, len, RNG_TIMEOUT_US, 1);
}

int rng_read_within(void *buf, size_t len, uint32_t limit_us)
{
    return rng_fill(buf, len, limit_us, 0);
}

int rng_u32(uint32_t *out)
{
    uint32_t w;
    int rc = rng_read(&w, sizeof(w));

    if (rc == 0) {
        *out = w;
    }
    return rc;
}

int rng_u32_within(uint32_t *out, uint32_t limit_us)
{
    uint32_t w;
    int rc = rng_read_within(&w, sizeof(w), limit_us);

    if (rc == 0) {
        *out = w;
    }
    return rc;
}
