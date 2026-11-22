#ifndef ROBCO_RNG_H
#define ROBCO_RNG_H

#include <stddef.h>
#include <stdint.h>

#define RNG_ERR_BOARD   (-1)    /* no peripheral base */
#define RNG_ERR_TIMEOUT (-2)    /* no data within the poll limit */

/* Starts the generator. Returns 0 or a negative error. */
int rng_init(void);

/* Fills buf with len random bytes. Blocks with a timeout; negative on error. */
int rng_read(void *buf, size_t len);

/* Stores one random word in *out. Returns 0, or a negative error and leaves *out alone. */
int rng_u32(uint32_t *out);

#endif
