#include <stdint.h>

#include <robco/input.h>

#include "rc_test.h"

static input_event_t ch(uint32_t n)
{
    input_event_t e = { INPUT_EV_CHAR, INPUT_SRC_SCRIPT, (uint16_t)n, (int32_t)n, 0 };

    return e;
}

static input_event_t dial(unsigned idx, int32_t steps)
{
    input_event_t e = { INPUT_EV_DIAL, INPUT_SRC_GPIO, (uint16_t)idx, steps, 0 };

    return e;
}

static input_event_t key(unsigned src, unsigned code, int32_t value)
{
    input_event_t e = { INPUT_EV_KEY, (uint8_t)src, (uint16_t)code, value, 0 };

    return e;
}

/* Pushes CHAR from..to, each expected to return `expect`. */
static void fill(input_core_t *c, uint32_t from, uint32_t to, int expect)
{
    uint32_t i;

    for (i = from; i <= to; i++) {
        input_event_t e = ch(i);
        CHECK_EQ(input_core_push(c, &e, 0), expect);
    }
}

TEST(core_init_defaults)
{
    input_core_t c;

    input_core_init(&c);
    CHECK_EQ(c.policy, INPUT_DROP_NEWEST);
    CHECK_EQ(input_queue_count(&c.queue), 0);
    CHECK_EQ(c.queue.dropped, 0);
    CHECK_EQ(input_core_dial_total(&c, 0), 0);
    CHECK_EQ(input_core_dial_total(&c, INPUT_DIAL_COUNT - 1), 0);
}

TEST(core_dial_totals)
{
    input_core_t c;
    input_event_t e;
    int pressed = 7;

    input_core_init(&c);
    e = dial(0, 3);
    CHECK_EQ(input_core_push(&c, &e, &pressed), 0);
    CHECK_EQ(pressed, 0);
    e = dial(0, -5);
    input_core_push(&c, &e, 0);
    e = dial(1, 7);
    input_core_push(&c, &e, 0);
    e = dial(1, 1);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, 0), -2);
    CHECK_EQ(input_core_dial_total(&c, 1), 8);
    /* Out of range: no total, no crash, and no effect on the others. */
    e = dial(INPUT_DIAL_COUNT, 100);
    input_core_push(&c, &e, 0);
    e = dial(0xffff, 100);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, INPUT_DIAL_COUNT), 0);
    CHECK_EQ(input_core_dial_total(&c, 0xffffffffu), 0);
    CHECK_EQ(input_core_dial_total(&c, 0), -2);
    CHECK_EQ(input_core_dial_total(&c, 1), 8);
    /* Other events do not touch the totals. */
    e = ch(0);
    input_core_push(&c, &e, 0);
    e = key(0, 0, 50);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, 0), -2);
}

TEST(core_dial_wraps)
{
    input_core_t c;
    input_event_t e;

    input_core_init(&c);
    e = dial(0, INT32_MAX);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, 0), INT32_MAX);
    e = dial(0, 1);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, 0), INT32_MIN);
    /* A difference across the wrap is still right. */
    CHECK_EQ((int32_t)((uint32_t)input_core_dial_total(&c, 0) - (uint32_t)INT32_MAX), 1);
    e = dial(0, -1);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, 0), INT32_MAX);

    e = dial(1, INT32_MIN);
    input_core_push(&c, &e, 0);
    e = dial(1, -1);
    input_core_push(&c, &e, 0);
    CHECK_EQ(input_core_dial_total(&c, 1), INT32_MAX);
}

static void dial_when_full(int policy, int expect)
{
    input_core_t c;
    input_event_t e;
    int32_t i;

    input_core_init(&c);
    c.policy = policy;
    fill(&c, 1, INPUT_QUEUE_LEN, 0);
    for (i = 1; i <= 10; i++) {
        e = dial(i & 1, -i);
        CHECK_EQ(input_core_push(&c, &e, 0), expect);
    }
    CHECK_EQ(input_core_dial_total(&c, 1), -(1 + 3 + 5 + 7 + 9));
    CHECK_EQ(input_core_dial_total(&c, 0), -(2 + 4 + 6 + 8 + 10));
    CHECK_EQ(c.queue.dropped, 10);
}

TEST(core_dial_totals_when_full)
{
    dial_when_full(INPUT_DROP_NEWEST, INPUT_ERR_FULL);
    dial_when_full(INPUT_DROP_OLDEST, INPUT_DROPPED_OLDEST);
}

TEST(core_drop_oldest_order)
{
    input_core_t c;
    input_event_t e;
    uint32_t i;

    input_core_init(&c);
    c.policy = INPUT_DROP_OLDEST;
    fill(&c, 1, 32, 0);
    fill(&c, 33, 40, INPUT_DROPPED_OLDEST);
    CHECK_EQ(input_queue_count(&c.queue), INPUT_QUEUE_LEN);
    CHECK_EQ(c.queue.dropped, 8);
    CHECK_EQ(c.queue.pushed, 40);
    for (i = 9; i <= 40; i++) {
        CHECK_EQ(input_queue_pop(&c.queue, &e), 1);
        CHECK_EQ(e.value, (int32_t)i);
    }
    CHECK_EQ(input_queue_pop(&c.queue, &e), 0);
}

TEST(core_drop_newest_order)
{
    input_core_t c;
    input_event_t e;
    uint32_t i;

    input_core_init(&c);
    fill(&c, 1, 32, 0);
    fill(&c, 33, 40, INPUT_ERR_FULL);
    CHECK_EQ(c.queue.dropped, 8);
    CHECK_EQ(c.queue.pushed, 32);
    for (i = 1; i <= 32; i++) {
        CHECK_EQ(input_queue_pop(&c.queue, &e), 1);
        CHECK_EQ(e.value, (int32_t)i);
    }
    CHECK_EQ(input_queue_pop(&c.queue, &e), 0);
}

static void keys_when_full(int policy)
{
    input_core_t c;
    input_event_t e;
    int pressed = 0;

    input_core_init(&c);
    c.policy = policy;
    fill(&c, 1, INPUT_QUEUE_LEN, 0);
    e = key(INPUT_SRC_GPIO, INPUT_KEY_UP, INPUT_PRESS);
    input_core_push(&c, &e, &pressed);
    CHECK_EQ(pressed, 1);
    e = key(INPUT_SRC_SCRIPT, INPUT_KEY_DOWN, INPUT_PRESS);
    input_core_push(&c, &e, 0);
    e = key(INPUT_SRC_SCRIPT, INPUT_KEY_DOWN, INPUT_RELEASE);
    input_core_push(&c, &e, &pressed);
    CHECK_EQ(pressed, 0);
    CHECK(input_hold_down(&c.hold, INPUT_KEY_UP));
    CHECK(!input_hold_down(&c.hold, INPUT_KEY_DOWN));
    CHECK_EQ(c.queue.dropped, 3);
}

TEST(core_keys_when_full)
{
    keys_when_full(INPUT_DROP_NEWEST);
    keys_when_full(INPUT_DROP_OLDEST);
}

TEST(core_init_resets)
{
    input_core_t c;
    input_event_t e;

    input_core_init(&c);
    c.policy = INPUT_DROP_OLDEST;
    fill(&c, 1, 32, 0);
    fill(&c, 33, 40, INPUT_DROPPED_OLDEST);
    e = dial(1, 9);
    input_core_push(&c, &e, 0);
    CHECK(c.queue.dropped > 0);
    input_core_init(&c);
    CHECK_EQ(c.policy, INPUT_DROP_NEWEST);
    CHECK_EQ(c.queue.dropped, 0);
    CHECK_EQ(input_queue_count(&c.queue), 0);
    CHECK_EQ(input_core_dial_total(&c, 1), 0);
    /* The default policy is back: a full queue refuses again. */
    fill(&c, 1, 32, 0);
    fill(&c, 33, 33, INPUT_ERR_FULL);
}
