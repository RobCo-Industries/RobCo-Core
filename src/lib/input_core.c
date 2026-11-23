#include <stdint.h>

#include <robco/input.h>

/*
 * The state behind src/drv/input.c, kept pure so the host tests reach it.
 * The caller provides mutual exclusion; src/drv/input.c masks IRQs.
 */

void input_core_init(input_core_t *c)
{
    unsigned i;

    input_queue_init(&c->queue);
    input_hold_init(&c->hold);
    for (i = 0; i < INPUT_DIAL_COUNT; i++) {
        c->dial_total[i] = 0;
    }
    c->policy = INPUT_DROP_NEWEST;
}

int input_core_push(input_core_t *c, const input_event_t *ev, int *pressed)
{
    int p = input_hold_update(&c->hold, ev);

    if (pressed) {
        *pressed = p;
    }
    if (ev->type == INPUT_EV_DIAL && ev->code < INPUT_DIAL_COUNT) {
        /* Unsigned, so the sum wraps without undefined behaviour. */
        c->dial_total[ev->code] += (uint32_t)ev->value;
    }
    if (c->policy == INPUT_DROP_OLDEST) {
        return input_queue_push_oldest(&c->queue, ev);
    }
    return input_queue_push(&c->queue, ev);
}

int32_t input_core_dial_total(const input_core_t *c, unsigned dial)
{
    return dial < INPUT_DIAL_COUNT ? (int32_t)c->dial_total[dial] : 0;
}

int input_core_room(const input_core_t *c, unsigned n)
{
    if (c->policy == INPUT_DROP_OLDEST) {
        return 1;
    }
    return INPUT_QUEUE_LEN - input_queue_count(&c->queue) >= n;
}
