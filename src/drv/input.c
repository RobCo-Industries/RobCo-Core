#include <stdint.h>

#include <robco/input.h>
#include <robco/irq.h>
#include <robco/tick.h>
#include <robco/timer.h>

/*
 * The shared queue, key state and auto-repeat. Producers run in IRQ or
 * main context, the consumer in main context. Every access to shared
 * state masks IRQs, so it is safe from both.
 */

#define MAX_POLLERS 4

static input_queue_t queue;
static uint32_t keys;           /* bit per key code */

static uint32_t rep_delay_ms;   /* 0: repeat off */
static uint32_t rep_rate_ms;
static uint16_t rep_code;       /* key being repeated, 0 for none */
static uint8_t rep_source;
static uint32_t rep_next_ms;
static int rep_registered;

static void (*pollers[MAX_POLLERS])(void);
static unsigned npollers;
static int in_poll;

static const char *const key_names[INPUT_KEY_COUNT] = {
    0, "up", "down", "left", "right", "select", "back",
    "f1", "f2", "f3", "f4", "dial_push",
};

const char *input_key_name(unsigned code)
{
    return code < INPUT_KEY_COUNT ? key_names[code] : 0;
}

int input_init(void)
{
    uint32_t s = irq_save();

    input_queue_init(&queue);
    keys = 0;
    rep_code = 0;
    irq_restore(s);
    return 0;
}

/* IRQs masked. */
static void track_keys(const input_event_t *ev)
{
    uint32_t bit;

    if (ev->type != INPUT_EV_KEY || ev->code >= 32) {
        return;
    }
    bit = (uint32_t)1 << ev->code;
    if (ev->value == INPUT_PRESS) {
        keys |= bit;
        rep_code = ev->code;
        rep_source = ev->source;
        rep_next_ms = ev->t_ms + rep_delay_ms;
    } else if (ev->value == INPUT_RELEASE) {
        keys &= ~bit;
        if (rep_code == ev->code) {
            rep_code = 0;
        }
    }
}

int input_push(const input_event_t *ev)
{
    input_event_t e = *ev;
    uint32_t s;
    int ret;

    e.t_ms = timer_now_ms();
    s = irq_save();
    track_keys(&e);
    ret = input_queue_push(&queue, &e);
    irq_restore(s);
    return ret;
}

/* Tick callback, IRQ context. */
static void repeat_tick(void)
{
    input_event_t e;
    uint32_t now;

    if (rep_delay_ms == 0 || rep_code == 0) {
        return;
    }
    now = timer_now_ms();
    if ((int32_t)(now - rep_next_ms) < 0) {
        return;
    }
    e.type = INPUT_EV_KEY;
    e.source = rep_source;
    e.code = rep_code;
    e.value = INPUT_REPEAT;
    e.t_ms = now;
    input_queue_push(&queue, &e);
    rep_next_ms += rep_rate_ms;
    /* After a stall, go on from now instead of sending a burst. */
    if ((int32_t)(now - rep_next_ms) >= 0) {
        rep_next_ms = now + rep_rate_ms;
    }
}

int input_set_repeat(uint32_t delay_ms, uint32_t rate_ms)
{
    uint32_t s;

    if (delay_ms != 0 && rate_ms == 0) {
        return INPUT_ERR_ARG;
    }
    if (delay_ms != 0 && !rep_registered) {
        if (tick_register(repeat_tick) != 0) {
            return INPUT_ERR_SPACE;
        }
        rep_registered = 1;
    }
    s = irq_save();
    rep_delay_ms = delay_ms;
    rep_rate_ms = rate_ms;
    rep_code = 0;
    irq_restore(s);
    return 0;
}

int input_add_poller(void (*fn)(void))
{
    if (!fn) {
        return INPUT_ERR_ARG;
    }
    if (npollers >= MAX_POLLERS) {
        return INPUT_ERR_SPACE;
    }
    pollers[npollers++] = fn;
    return 0;
}

static void run_pollers(void)
{
    unsigned i;

    if (in_poll) {
        return;
    }
    in_poll = 1;
    for (i = 0; i < npollers; i++) {
        pollers[i]();
    }
    in_poll = 0;
}

static int take(input_event_t *ev)
{
    uint32_t s = irq_save();
    int got = input_queue_pop(&queue, ev);

    irq_restore(s);
    return got;
}

int input_poll(input_event_t *ev)
{
    run_pollers();
    return take(ev);
}

int input_wait(input_event_t *ev, uint32_t timeout_ms)
{
    uint32_t start = timer_now_ms();
    uint32_t s;

    for (;;) {
        if (input_poll(ev)) {
            return 1;
        }
        if (timeout_ms != INPUT_WAIT_FOREVER && timer_now_ms() - start >= timeout_ms) {
            return 0;
        }
        /*
         * Sleep until the next interrupt. IRQs stay masked across the
         * check and the wfi, so an event pushed by a handler in between
         * still wakes it. Without the tick no interrupt may ever come, so
         * spin instead.
         */
        s = irq_save();
        if (!s && tick_count() != 0 && queue.head == queue.tail) {
            __asm__ volatile("wfi" : : : "memory");
        }
        irq_restore(s);
    }
}

unsigned input_pending(void)
{
    uint32_t s = irq_save();
    unsigned n = input_queue_count(&queue);

    irq_restore(s);
    return n;
}

void input_flush(void)
{
    uint32_t s = irq_save();

    queue.tail = queue.head;
    irq_restore(s);
}

void input_stats(uint32_t *pushed, uint32_t *dropped)
{
    uint32_t s = irq_save();

    if (pushed) {
        *pushed = queue.pushed;
    }
    if (dropped) {
        *dropped = queue.dropped;
    }
    irq_restore(s);
}

int input_key_down(unsigned code)
{
    return code < 32 && (keys >> code) & 1;
}
