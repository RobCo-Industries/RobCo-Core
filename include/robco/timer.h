#ifndef ROBCO_TIMER_H
#define ROBCO_TIMER_H

#include <stdint.h>

/*
 * BCM283x system timer: a free-running 1 MHz counter with four compare
 * channels. Channels 0 and 2 belong to the GPU firmware. Channel 1 is
 * used by tick.c. Channel 3 is free for callers.
 */

/* Microseconds since the counter started (CHI:CLO read consistently). */
uint64_t timer_now_us(void);

/* Milliseconds since the counter started, wraps after about 49 days. */
uint32_t timer_now_ms(void);

/* Low 32 bits of the counter (CLO). Wraps after about 71 minutes. */
uint32_t timer_now_lo(void);

/* Busy-waits at least `us` microseconds. Correct up to UINT32_MAX. */
void timer_delay_us(uint32_t us);

/* Busy-waits at least `ms` milliseconds. Correct up to UINT32_MAX (49 days). */
void timer_delay_ms(uint32_t ms);

/*
 * Non-zero once timer_now_us() has reached `deadline_us`. The 64-bit
 * counter does not wrap in practice, so a plain compare is exact: 0 has
 * always expired and UINT64_MAX never does.
 */
int timer_expired(uint64_t deadline_us);

/* Sets compare channel `ch` (0..3) to match when CLO equals `clo`. */
void timer_compare_set(unsigned ch, uint32_t clo);

/* First and largest retry distance of timer_oneshot_arm(), in microseconds. */
#define TIMER_ARM_MIN_US    16u
#define TIMER_ARM_MAX_US    65536u

/*
 * Sets channel `ch` to `clo`, then clears its match flag. Returns 0 when
 * the interrupt will come: the match is still ahead or has just happened.
 * Returns 1 when CLO passed `clo` before the write took effect: the match
 * was missed and the caller must handle the deadline itself.
 */
int timer_compare_arm(unsigned ch, uint32_t clo);

/*
 * Arms a one-shot match on channel `ch` at `clo`. If `clo` is already
 * missed, arms a short distance ahead instead and checks again, with a
 * growing distance. Returns the value armed. The interrupt always comes.
 */
uint32_t timer_oneshot_arm(unsigned ch, uint32_t clo);

/* Clears the match flag of channel `ch` (write 1 to CS). */
void timer_compare_clear(unsigned ch);

/* Non-zero if channel `ch` has matched and is not cleared yet. */
int timer_compare_pending(unsigned ch);

#endif
