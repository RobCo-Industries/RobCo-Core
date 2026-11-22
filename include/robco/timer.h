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

/* Busy-waits at least `us` microseconds. */
void timer_delay_us(uint32_t us);

/* Busy-waits at least `ms` milliseconds. */
void timer_delay_ms(uint32_t ms);

/* Non-zero once timer_now_us() has reached `deadline_us`. */
int timer_expired(uint64_t deadline_us);

/* Sets compare channel `ch` (0..3) to match when CLO equals `clo`. */
void timer_compare_set(unsigned ch, uint32_t clo);

/* Clears the match flag of channel `ch` (write 1 to CS). */
void timer_compare_clear(unsigned ch);

/* Non-zero if channel `ch` has matched and is not cleared yet. */
int timer_compare_pending(unsigned ch);

#endif
