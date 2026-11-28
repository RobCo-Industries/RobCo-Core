#ifndef ROBCO_RNG_H
#define ROBCO_RNG_H

#include <stddef.h>
#include <stdint.h>

#define RNG_ERR_BOARD   (-1)    /* no peripheral base */
#define RNG_ERR_TIMEOUT (-2)    /* no word within the time limit */

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
 * the warm-up. A call for several words can wait that long for each one.
 * Returns 0 or a negative error. On an error, buf holds the bytes of the
 * words read before the failed one and the rest of buf is unchanged.
 * The wait needs the system timer. A timer that does not advance ends it
 * with RNG_ERR_TIMEOUT after a bounded number of reads, so the call never
 * hangs. That guard counts reads, not time: on a stopped timer the wait
 * lasts as long as a million reads take.
 *
 * A caller that must be back in its main loop soon, as to kick a
 * watchdog, uses rng_read_within() instead.
 */
int rng_read(void *buf, size_t len);

/*
 * As rng_read(), but the call waits for words at most limit_us
 * microseconds of the system timer, counted from its start. The limit
 * bounds waiting only. A word that is ready is taken with no wait, also
 * past the limit and also with limit_us 0. So a call that finds every
 * word ready reads all len / 4 words whatever the limit, and a call runs
 * for at most limit_us plus the time to read its ready words. limit_us 0
 * never waits: each word is taken only if it is ready at its first
 * status read. After the limit the status is read once more, so a word
 * that came in the meantime is still taken. Returns 0, RNG_ERR_BOARD or
 * RNG_ERR_TIMEOUT, and leaves buf as rng_read() does on an error. The
 * stall guard of rng_read() applies to each word: it counts reads, not
 * time, so on a stopped timer the call can last longer than limit_us.
 * A caller that must be back in its main loop soon, as to kick a
 * watchdog, keeps len small and picks limit_us well under the watchdog
 * timeout.
 */
int rng_read_within(void *buf, size_t len, uint32_t limit_us);

/* Stores one random word in *out. Returns 0, or a negative error and leaves *out alone. */
int rng_u32(uint32_t *out);

/*
 * As rng_u32(), with the limit of rng_read_within(). Returns 0, or a
 * negative error and leaves *out alone.
 */
int rng_u32_within(uint32_t *out, uint32_t limit_us);

#endif
