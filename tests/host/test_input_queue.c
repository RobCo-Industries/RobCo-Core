#include <stdint.h>

#include <robco/input.h>

#include "rc_test.h"

static input_event_t ev_n(uint32_t n)
{
    input_event_t e = { INPUT_EV_CHAR, INPUT_SRC_SCRIPT, (uint16_t)(n & 0xffff), (int32_t)n, n };

    return e;
}

TEST(queue_empty)
{
    input_queue_t q;
    input_event_t e;

    input_queue_init(&q);
    CHECK_EQ(input_queue_count(&q), 0);
    CHECK_EQ(input_queue_pop(&q, &e), 0);
    CHECK_EQ(q.pushed, 0);
    CHECK_EQ(q.dropped, 0);
}

TEST(queue_order)
{
    input_queue_t q;
    input_event_t e;
    uint32_t i;

    input_queue_init(&q);
    for (i = 0; i < 10; i++) {
        input_event_t in = ev_n(i);
        CHECK_EQ(input_queue_push(&q, &in), 0);
    }
    CHECK_EQ(input_queue_count(&q), 10);
    for (i = 0; i < 10; i++) {
        CHECK_EQ(input_queue_pop(&q, &e), 1);
        CHECK_EQ(e.value, i);
        CHECK_EQ(e.t_ms, i);
        CHECK_EQ(e.type, INPUT_EV_CHAR);
        CHECK_EQ(e.source, INPUT_SRC_SCRIPT);
    }
    CHECK_EQ(input_queue_pop(&q, &e), 0);
}

TEST(queue_overflow_drops_newest)
{
    input_queue_t q;
    input_event_t e;
    uint32_t i;

    input_queue_init(&q);
    for (i = 0; i < INPUT_QUEUE_LEN; i++) {
        input_event_t in = ev_n(i);
        CHECK_EQ(input_queue_push(&q, &in), 0);
    }
    for (i = 0; i < 5; i++) {
        input_event_t in = ev_n(1000 + i);
        CHECK_EQ(input_queue_push(&q, &in), INPUT_ERR_FULL);
    }
    CHECK_EQ(q.pushed, INPUT_QUEUE_LEN);
    CHECK_EQ(q.dropped, 5);
    CHECK_EQ(input_queue_count(&q), INPUT_QUEUE_LEN);
    /* The oldest events survive, in order. */
    for (i = 0; i < INPUT_QUEUE_LEN; i++) {
        CHECK_EQ(input_queue_pop(&q, &e), 1);
        CHECK_EQ(e.value, i);
    }
    CHECK_EQ(input_queue_pop(&q, &e), 0);
}

TEST(queue_wrap)
{
    input_queue_t q;
    input_event_t e;
    uint32_t next_in = 0, next_out = 0;
    unsigned round;

    input_queue_init(&q);
    /* Push 3, pop 2, many times: indices wrap the ring often. */
    for (round = 0; round < 500; round++) {
        unsigned k;

        for (k = 0; k < 3; k++) {
            input_event_t in = ev_n(next_in);
            if (input_queue_push(&q, &in) == 0) {
                next_in++;
            }
        }
        for (k = 0; k < 2; k++) {
            if (input_queue_pop(&q, &e)) {
                CHECK_EQ(e.value, next_out);
                next_out++;
            }
        }
        CHECK(input_queue_count(&q) <= INPUT_QUEUE_LEN);
    }
    while (input_queue_pop(&q, &e)) {
        CHECK_EQ(e.value, next_out);
        next_out++;
    }
    CHECK_EQ(next_out, next_in);
    CHECK_EQ(q.pushed + q.dropped, 1500);
    CHECK(q.dropped > 0);
}

TEST(queue_counter_wrap)
{
    input_queue_t q;
    input_event_t e;
    input_event_t in = ev_n(7);

    input_queue_init(&q);
    /* Free-running indices near 2^32 keep working. */
    q.head = q.tail = 0xfffffffeu;
    CHECK_EQ(input_queue_push(&q, &in), 0);
    CHECK_EQ(input_queue_push(&q, &in), 0);
    CHECK_EQ(input_queue_push(&q, &in), 0);
    CHECK_EQ(input_queue_count(&q), 3);
    CHECK_EQ(input_queue_pop(&q, &e), 1);
    CHECK_EQ(input_queue_pop(&q, &e), 1);
    CHECK_EQ(input_queue_pop(&q, &e), 1);
    CHECK_EQ(input_queue_pop(&q, &e), 0);
    CHECK_EQ(e.value, 7);
}
