#include <stdint.h>

#include <robco/irq.h>
#include <robco/tick.h>
#include <robco/timer.h>

#define TICK_CHANNEL    1

static volatile uint32_t count;
static volatile uint32_t missed;
static uint32_t period_us;
static uint32_t tick_hz;
static uint32_t next_match;     /* CLO value currently in C1 */
static void (*volatile callbacks[TICK_MAX_CALLBACKS])(void);

static void tick_run_callbacks(void)
{
    unsigned i;
    void (*fn)(void);

    for (i = 0; i < TICK_MAX_CALLBACKS; i++) {
        fn = callbacks[i];
        if (fn) {
            fn();
        }
    }
}

/*
 * Counts every period from next_match up to now and moves next_match past
 * now, staying on the period grid. Returns the number of periods counted,
 * 0 if next_match is still ahead.
 */
static uint32_t tick_catch_up(void)
{
    uint32_t late = timer_now_lo() - next_match;
    uint32_t n;

    if ((int32_t)late < 0) {
        return 0;
    }
    n = late / period_us + 1;
    next_match += n * period_us;
    count += n;
    return n;
}

static void tick_isr(unsigned irq, void *arg)
{
    uint32_t n;

    (void)irq;
    (void)arg;

    timer_compare_clear(TICK_CHANNEL);
    /*
     * Re-arm relative to the previous match, not to "now", so the period
     * does not drift. Callbacks run at most once per interrupt: if they
     * or a long masked section made the handler fall behind, the skipped
     * periods are counted in one step and only tick_missed() grows. This
     * bounds the handler even when a callback takes longer than a period.
     * The re-check after writing C1 covers a deadline that passed during
     * the write: the comparator only fires on equality, so a past value
     * would wait a full 71 minute wrap. A stale match flag just causes a
     * call that counts nothing.
     */
    n = tick_catch_up();
    if (n) {
        missed += n - 1;
        tick_run_callbacks();
    }
    for (;;) {
        timer_compare_set(TICK_CHANNEL, next_match);
        if ((int32_t)(timer_now_lo() - next_match) < 0) {
            break;
        }
        missed += tick_catch_up();
    }
}

int tick_init(uint32_t hz)
{
    uint32_t s;

    if (hz == 0 || hz > 10000 || period_us) {
        return -1;
    }
    if (irq_register(IRQ_TIMER1, tick_isr, 0)) {
        return -1;
    }
    s = irq_save();
    tick_hz = hz;
    period_us = 1000000u / hz;
    count = 0;
    missed = 0;
    next_match = timer_now_lo() + period_us;
    timer_compare_set(TICK_CHANNEL, next_match);
    timer_compare_clear(TICK_CHANNEL);
    irq_enable(IRQ_TIMER1);
    irq_restore(s);
    return 0;
}

uint32_t tick_count(void)
{
    return count;
}

uint32_t tick_missed(void)
{
    return missed;
}

uint32_t tick_ms(void)
{
    if (!tick_hz) {
        return 0;
    }
    return (uint32_t)((uint64_t)count * 1000u / tick_hz);
}

int tick_register(void (*fn)(void))
{
    unsigned i;
    uint32_t s;
    int ret = -1;

    s = irq_save();
    for (i = 0; i < TICK_MAX_CALLBACKS; i++) {
        if (!callbacks[i]) {
            callbacks[i] = fn;
            ret = 0;
            break;
        }
    }
    irq_restore(s);
    return ret;
}
