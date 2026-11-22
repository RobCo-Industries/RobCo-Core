#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/mmio.h>
#include <robco/rng.h>

#define RNG_OFFSET      0x104000
#define RNG_CTRL        0x00
#define RNG_STATUS      0x04
#define RNG_DATA        0x08

#define RNG_ENABLE      1u
#define RNG_WARMUP      0x40000u
#define RNG_POLL_LIMIT  1000000u

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
        /* Discard the first words while the generator warms up. */
        mmio_write(base + RNG_STATUS, RNG_WARMUP);
        mmio_write(base + RNG_CTRL, RNG_ENABLE);
    }
    return 0;
}

/* Waits for one word. Returns 0 and stores it, or a negative error. */
static int rng_word(uint32_t base, uint32_t *out)
{
    uint32_t n;

    for (n = 0; n < RNG_POLL_LIMIT; n++) {
        if (mmio_read(base + RNG_STATUS) >> 24) {
            *out = mmio_read(base + RNG_DATA);
            return 0;
        }
    }
    return RNG_ERR_TIMEOUT;
}

int rng_read(void *buf, size_t len)
{
    uint32_t base = rng_base();
    unsigned char *p = buf;
    uint32_t w;
    size_t i;
    int rc;

    if (base == 0) {
        return RNG_ERR_BOARD;
    }
    while (len > 0) {
        rc = rng_word(base, &w);
        if (rc < 0) {
            return rc;
        }
        for (i = 0; i < 4 && len > 0; i++, len--) {
            *p++ = (unsigned char)(w >> (i * 8));
        }
    }
    return 0;
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
