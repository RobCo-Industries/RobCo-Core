#include <stdint.h>

#include <robco/input.h>

/*
 * Stable-count filter. Each line keeps a counter of consecutive samples
 * that disagree with its debounced state. Any sample that agrees resets
 * the counter, so a glitch shorter than `stable` samples never shows.
 * When the counter reaches `stable`, the state flips and one edge is
 * reported.
 */

void input_debounce_init(input_debounce_t *d, unsigned stable_samples, uint32_t active_low)
{
    unsigned i;

    if (stable_samples < 1) {
        stable_samples = 1;
    }
    if (stable_samples > 255) {
        stable_samples = 255;
    }
    d->active_low = active_low;
    d->state = 0;
    d->stable = (uint8_t)stable_samples;
    for (i = 0; i < INPUT_DEBOUNCE_LINES; i++) {
        d->count[i] = 0;
    }
}

uint32_t input_debounce_feed(input_debounce_t *d, uint32_t levels,
                             uint32_t *pressed, uint32_t *released)
{
    uint32_t raw = levels ^ d->active_low;
    uint32_t diff = raw ^ d->state;
    uint32_t flip = 0;
    unsigned i;

    for (i = 0; i < INPUT_DEBOUNCE_LINES; i++) {
        uint32_t bit = (uint32_t)1 << i;

        if (!(diff & bit)) {
            d->count[i] = 0;
            continue;
        }
        if (++d->count[i] >= d->stable) {
            d->count[i] = 0;
            flip |= bit;
        }
    }
    d->state ^= flip;
    *pressed = flip & d->state;
    *released = flip & ~d->state;
    return d->state;
}

unsigned input_debounce_samples(unsigned ms, unsigned hz)
{
    uint64_t n;

    if (hz == 0) {
        return 1;
    }
    n = ((uint64_t)ms * hz + 999) / 1000;
    if (n < 1) {
        n = 1;
    }
    if (n > 255) {
        n = 255;
    }
    return (unsigned)n;
}
