#include <stdint.h>

#include <robco/irq.h>
#include <robco/tick.h>
#include <robco/timer.h>

#define TICK_CHANNEL    1

static volatile uint32_t count;
static volatile uint32_t wraps;      /* tick_count() wraps, for tick_ms() */
static volatile uint32_t missed;
static uint32_t period_us;
static uint32_t next_match;     /* next period boundary not yet counted */
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
    if (count + n < count) {
        wraps++;
    }
    count += n;
    return n;
}

/*
 * Arms C1 for next_match. If the deadline passed before the write took
 * effect, counts the elapsed periods as missed and tries again. Each retry
 * arms a few more periods ahead, so the loop ends even if every write is
 * slow. catch_up() counts the skipped boundaries once they pass.
 */
static void tick_arm(void)
{
    uint32_t ahead = 0;

    while (timer_compare_arm(TICK_CHANNEL, next_match + ahead)) {
        missed += tick_catch_up();
        if (ahead < TIMER_ARM_MAX_US) {
            ahead += ahead ? ahead : period_us;
        }
    }
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
     */
    n = tick_catch_up();
    if (n) {
        missed += n - 1;
        tick_run_callbacks();
    }
    tick_arm();
}

int tick_init(uint32_t hz)
{
    uint32_t s;
    uint32_t p = tick_period_us_for(hz);

    if (p == 0) {
        return TICK_ERR_RANGE;
    }
    if (period_us) {
        return p == period_us ? 0 : TICK_ERR_RUNNING;
    }
    if (!irq_ready()) {
        return TICK_ERR_NO_IRQ;
    }
    if (irq_register(IRQ_TIMER1, tick_isr, 0)) {
        return TICK_ERR_IRQ_TAKEN;
    }
    s = irq_save();
    period_us = p;
    count = 0;
    wraps = 0;
    missed = 0;
    next_match = timer_now_lo() + period_us;
    tick_arm();
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
    uint64_t ticks;
    uint32_t s;

    s = irq_save();
    ticks = ((uint64_t)wraps << 32) | count;
    irq_restore(s);
    /* Truncation wraps the result at 2^32 ms, as the return type says. */
    return (uint32_t)tick_to_ms(ticks, period_us);
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
