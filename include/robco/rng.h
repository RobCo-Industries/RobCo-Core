#ifndef ROBCO_RNG_H
#define ROBCO_RNG_H

#include <stddef.h>
#include <stdint.h>

#define RNG_ERR_BOARD   (-1)    /* no peripheral base */
#define RNG_ERR_TIMEOUT (-2)    /* no word within RNG_TIMEOUT_US */

/*
 * Longest wait for one word, in microseconds of the system timer. The
 * real generator gives no word while it warms up after its start. This
 * limit is meant to be several times that, so a timeout means a
 * generator that gives no word. docs/HARDWARE_VALIDATION.md tells how to
 * measure the warm-up on a board.
 */
#define RNG_TIMEOUT_US  3000000u

/*
 * Starts the generator if it is off and returns at once: no wait, no use
 * of the timer. A generator that already runs is left alone. Returns 0 or
 * RNG_ERR_BOARD.
 */
int rng_init(void);

/*
 * Fills buf with len random bytes. Busy-waits for each word, up to
 * RNG_TIMEOUT_US: the first read after rng_init() waits for the rest of
 * the warm-up. Returns 0 or a negative error. The wait needs the system
 * timer. A timer that does not advance ends it with RNG_ERR_TIMEOUT
 * after a bounded number of reads, so the call never hangs.
 */
int rng_read(void *buf, size_t len);

/* Stores one random word in *out. Returns 0, or a negative error and leaves *out alone. */
int rng_u32(uint32_t *out);

#endif
