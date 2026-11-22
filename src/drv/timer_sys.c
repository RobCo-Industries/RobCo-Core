#include <stdint.h>

#include <robco/board.h>
#include <robco/mmio.h>
#include <robco/timer.h>

#define ST_OFFSET   0x3000
#define ST_CS       0x00
#define ST_CLO      0x04
#define ST_CHI      0x08
#define ST_C0       0x0c

static uint32_t st_base(void)
{
    return board_get()->periph_base + ST_OFFSET;
}

uint64_t timer_now_us(void)
{
    uint32_t base = st_base();
    uint32_t hi;
    uint32_t lo;

    /* CLO can wrap between the two reads: retry until CHI is stable. */
    do {
        hi = mmio_read(base + ST_CHI);
        lo = mmio_read(base + ST_CLO);
    } while (mmio_read(base + ST_CHI) != hi);
    return ((uint64_t)hi << 32) | lo;
}

uint32_t timer_now_ms(void)
{
    return (uint32_t)(timer_now_us() / 1000);
}

uint32_t timer_now_lo(void)
{
    return mmio_read(st_base() + ST_CLO);
}

/*
 * Waits until the 64-bit counter passes `start + us`. The counter wraps
 * after about 584000 years, so any 64-bit span below that is safe. Wait
 * one extra count: `start` may have been read just before CLO stepped.
 */
static void delay_span(uint64_t us)
{
    uint64_t end = timer_now_us() + us;

    while (timer_now_us() <= end) {
    }
}

void timer_delay_us(uint32_t us)
{
    delay_span(us);
}

void timer_delay_ms(uint32_t ms)
{
    delay_span((uint64_t)ms * 1000u);
}

int timer_expired(uint64_t deadline_us)
{
    return timer_now_us() >= deadline_us;
}

void timer_compare_set(unsigned ch, uint32_t clo)
{
    if (ch < 4) {
        mmio_write(st_base() + ST_C0 + 4 * ch, clo);
    }
}

void timer_compare_clear(unsigned ch)
{
    uint32_t base = st_base();

    if (ch < 4) {
        mmio_write(base + ST_CS, 1u << ch);
        /*
         * Read back so the clear reaches the timer before the IRQ handler
         * returns. Otherwise the CPU can see the old level and re-enter.
         */
        (void)mmio_read(base + ST_CS);
    }
}

int timer_compare_pending(unsigned ch)
{
    return ch < 4 && (mmio_read(st_base() + ST_CS) & (1u << ch)) != 0;
}
