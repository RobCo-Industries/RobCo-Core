#include <stdint.h>

#include <robco/input.h>

#include "rc_test.h"

/* Clockwise Gray sequence of (A << 1) | B. */
static const int cw[4] = { 0, 1, 3, 2 };

/* Feeds states given as digits 0..3 and sums the detent steps. */
static int run(input_quad_t *q, const char *states)
{
    int sum = 0;
    int i;

    for (i = 0; states[i]; i++) {
        int s = states[i] - '0';
        sum += input_quad_feed(q, (s >> 1) & 1, s & 1);
    }
    return sum;
}

/* Steps `n` transitions from Gray position *pos, positive is clockwise. */
static int turn(input_quad_t *q, int *pos, int n)
{
    int sum = 0;

    while (n != 0) {
        int dir = n > 0 ? 1 : -1;
        int s;

        *pos = (*pos + dir + 4) % 4;
        s = cw[*pos];
        sum += input_quad_feed(q, (s >> 1) & 1, s & 1);
        n -= dir;
    }
    return sum;
}

TEST(quad_clean_both_ways)
{
    input_quad_t q;
    int pos = 0;

    input_quad_init(&q, 4, 0, 0, 0);
    CHECK_EQ(turn(&q, &pos, 4), 1);
    CHECK_EQ(turn(&q, &pos, 12), 3);
    CHECK_EQ(turn(&q, &pos, -8), -2);
    CHECK_EQ(q.invalid, 0);
    CHECK_EQ(q.acc, 0);
}

TEST(quad_one_detent_emits_once)
{
    input_quad_t q;

    input_quad_init(&q, 4, 0, 0, 0);
    /* 00 01 11 10 00: one detent, reported on the last transition only. */
    CHECK_EQ(input_quad_feed(&q, 0, 1), 0);
    CHECK_EQ(input_quad_feed(&q, 1, 1), 0);
    CHECK_EQ(input_quad_feed(&q, 1, 0), 0);
    CHECK_EQ(input_quad_feed(&q, 0, 0), 1);
}

TEST(quad_direction_reverse)
{
    input_quad_t q;
    int pos = 0;

    input_quad_init(&q, 4, 1, 0, 0);
    CHECK_EQ(turn(&q, &pos, 8), -2);
    CHECK_EQ(turn(&q, &pos, -4), 1);
}

TEST(quad_per_detent)
{
    input_quad_t q;
    int pos = 0;

    input_quad_init(&q, 1, 0, 0, 0);
    CHECK_EQ(turn(&q, &pos, 5), 5);
    pos = 0;
    input_quad_init(&q, 2, 0, 0, 0);
    CHECK_EQ(turn(&q, &pos, 6), 3);
    CHECK_EQ(turn(&q, &pos, -2), -1);
    input_quad_init(&q, 3, 0, 0, 0);
    CHECK_EQ(q.per_detent, 4);
}

TEST(quad_bounce_on_one_channel)
{
    input_quad_t q;

    input_quad_init(&q, 4, 0, 0, 0);
    /* B chatters 0/1 while A stays 0, then the turn completes. */
    CHECK_EQ(run(&q, "0101010101"), 0);
    CHECK_EQ(run(&q, "1"), 0);
    /* Then A chatters at 11/01, still no step, then finish 3 -> 2 -> 0. */
    CHECK_EQ(run(&q, "3131313"), 0);
    CHECK_EQ(run(&q, "20"), 1);
    CHECK_EQ(q.invalid, 0);
}

TEST(quad_reversal_mid_detent)
{
    input_quad_t q;
    int pos = 0;

    input_quad_init(&q, 4, 0, 0, 0);
    /* Two transitions forward, two back: nothing. */
    CHECK_EQ(turn(&q, &pos, 2), 0);
    CHECK_EQ(turn(&q, &pos, -2), 0);
    CHECK_EQ(q.acc, 0);
    /* Three forward, then all the way back one detent: -1 only. */
    CHECK_EQ(turn(&q, &pos, 3), 0);
    CHECK_EQ(turn(&q, &pos, -7), -1);
    CHECK_EQ(q.acc, 0);
}

TEST(quad_fast_rotation)
{
    input_quad_t q;
    int pos = 0;

    /* One transition per sample, no gaps: every detent counts. */
    input_quad_init(&q, 4, 0, 0, 0);
    CHECK_EQ(turn(&q, &pos, 400), 100);
    CHECK_EQ(turn(&q, &pos, -400), -100);
    CHECK_EQ(q.invalid, 0);
}

TEST(quad_illegal_double_transition)
{
    input_quad_t q;

    input_quad_init(&q, 4, 0, 0, 0);
    /* 00 -> 11 skips a state: rejected, counted, no step. */
    CHECK_EQ(input_quad_feed(&q, 1, 1), 0);
    CHECK_EQ(q.invalid, 1);
    CHECK_EQ(q.acc, 0);
    /* 11 -> 00 too. */
    CHECK_EQ(run(&q, "0"), 0);
    CHECK_EQ(q.invalid, 2);
    /* 00 -> 01 is legal (+1), then 01 <-> 10 three times is not. */
    CHECK_EQ(run(&q, "1212"), 0);
    CHECK_EQ(q.invalid, 5);
    CHECK_EQ(q.acc, 1);
    /* Decoding goes on from the new state: 10 00 01 11 10 is one detent clockwise. */
    CHECK_EQ(run(&q, "0132"), 1);
    CHECK_EQ(q.acc, 1);
}

TEST(quad_initial_state)
{
    input_quad_t q;

    /* Starting at 11 (pull-ups at rest) does not count a step. */
    input_quad_init(&q, 4, 0, 1, 1);
    CHECK_EQ(input_quad_feed(&q, 1, 1), 0);
    CHECK_EQ(run(&q, "2013"), 1);
}
