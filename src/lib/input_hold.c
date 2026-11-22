#include <stdint.h>

#include <robco/input.h>

/*
 * Held keys per source. A key is down while any source holds it. The
 * caller provides mutual exclusion; src/drv/input.c masks IRQs.
 */

void input_hold_init(input_hold_t *h)
{
    unsigned i;

    for (i = 0; i < INPUT_HOLD_SOURCES; i++) {
        h->held[i] = 0;
    }
    h->rep_code = 0;
    h->rep_source = 0;
}

int input_hold_update(input_hold_t *h, const input_event_t *ev)
{
    uint32_t bit;
    unsigned s;

    if (ev->type != INPUT_EV_KEY || ev->code >= 32 || ev->source >= INPUT_HOLD_SOURCES) {
        return 0;
    }
    bit = (uint32_t)1 << ev->code;
    if (ev->value == INPUT_PRESS) {
        h->held[ev->source] |= bit;
        h->rep_code = ev->code;
        h->rep_source = ev->source;
        return 1;
    }
    if (ev->value != INPUT_RELEASE || !(h->held[ev->source] & bit)) {
        return 0;
    }
    h->held[ev->source] &= ~bit;
    if (h->rep_code == ev->code && h->rep_source == ev->source) {
        /* Hand the repeat to another source that still holds the key. */
        h->rep_code = 0;
        for (s = 0; s < INPUT_HOLD_SOURCES; s++) {
            if (h->held[s] & bit) {
                h->rep_code = ev->code;
                h->rep_source = (uint8_t)s;
                break;
            }
        }
    }
    return 0;
}

int input_hold_down(const input_hold_t *h, unsigned code)
{
    uint32_t any = 0;
    unsigned i;

    if (code >= 32) {
        return 0;
    }
    for (i = 0; i < INPUT_HOLD_SOURCES; i++) {
        any |= h->held[i];
    }
    return (any >> code) & 1;
}
