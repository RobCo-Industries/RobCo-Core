#ifndef ROBCO_TICK_H
#define ROBCO_TICK_H

#include <stdint.h>

#define TICK_MAX_CALLBACKS  4

/* Accepted tick rates. The period is never below 100 us. */
#define TICK_HZ_MIN         1u
#define TICK_HZ_MAX         10000u

/* tick_init() errors. */
#define TICK_ERR_RANGE      (-1)    /* hz outside TICK_HZ_MIN..TICK_HZ_MAX */
#define TICK_ERR_RUNNING    (-2)    /* the tick already runs at another rate */
#define TICK_ERR_NO_IRQ     (-3)    /* irq_init() has not run */
#define TICK_ERR_IRQ_TAKEN  (-4)    /* another handler owns IRQ_TIMER1 */

/*
 * Starts a periodic tick at `hz` on system timer channel 1 (IRQ 1).
 * Needs irq_init() first. IRQs on the CPU stay as they are.
 * The period is 1000000 / hz microseconds, rounded down, so a rate that
 * does not divide 1000000 runs a little fast: 7000 Hz gives 142 us, or
 * about 7042 Hz. tick_ms() uses the real period, so it stays exact.
 * Returns 0 or a TICK_ERR_* code. A failed call changes nothing, so it
 * can be retried, for example after irq_init(). A call while the tick runs
 * returns 0 for the same rate and TICK_ERR_RUNNING for another one.
 */
int tick_init(uint32_t hz);

/*
 * The rate tick_init() started, in Hz, or 0 when the tick does not run.
 * The real rate can be a little higher: see tick_init().
 */
uint32_t tick_hz(void);

/* Ticks since tick_init(). */
uint32_t tick_count(void);

/*
 * Ticks counted without a callback run. The handler runs the callbacks at
 * most once per interrupt, so a slow callback or a long masked section
 * makes this grow. tick_count() still counts every elapsed period.
 */
uint32_t tick_missed(void);

/*
 * Milliseconds since tick_init(), from the tick count and the programmed
 * period. Counts past a tick_count() wrap and wraps itself at 2^32 ms.
 * Not for IRQ context: it uses a 64-bit division.
 */
uint32_t tick_ms(void);

/* Adds a callback run in IRQ context once per tick interrupt. Returns 0 or -1 if full. */
int tick_register(void (*fn)(void));

/* Pure functions, src/lib/tick_conv.c. */

/* The period in microseconds for `hz`, or 0 if `hz` is out of range. */
uint32_t tick_period_us_for(uint32_t hz);

/* `ticks` periods of `period_us` in milliseconds, rounded down. Exact for ticks below 2^44. */
uint64_t tick_to_ms(uint64_t ticks, uint32_t period_us);

#endif
