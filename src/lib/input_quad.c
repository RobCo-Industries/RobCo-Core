#include <stdint.h>

#include <robco/input.h>

/*
 * Gray-code state table, indexed by (prev << 2) | cur with each state
 * (A << 1) | B. Clockwise runs 00 -> 01 -> 11 -> 10 -> 00. A change of
 * one bit is a step of +1 or -1. A change of both bits means a sample
 * was missed: 2 marks it.
 */
#define BAD 2

static const int8_t quad_table[16] = {
    /* prev 00 */ 0, 1, -1, BAD,
    /* prev 01 */ -1, 0, BAD, 1,
    /* prev 10 */ 1, BAD, 0, -1,
    /* prev 11 */ BAD, -1, 1, 0,
};

/* Keeps acc far from int8_t overflow when noise piles up between rests. */
#define ACC_LIMIT 8

void input_quad_init(input_quad_t *q, unsigned per_detent, int reverse, int a, int b)
{
    if (per_detent != 1 && per_detent != 2) {
        per_detent = 4;
    }
    q->per_detent = (uint8_t)per_detent;
    q->reverse = reverse ? 1 : 0;
    q->prev = (uint8_t)(((a ? 1 : 0) << 1) | (b ? 1 : 0));
    q->rest = q->prev;
    q->dir = 0;
    q->pend = 0;
    q->acc = 0;
    q->invalid = 0;
}

/* A rest state: the start state, and with 2 per detent its complement too. */
static int at_rest(const input_quad_t *q, unsigned s)
{
    return s == q->rest || (q->per_detent == 2 && s == (q->rest ^ 3u));
}

int input_quad_feed(input_quad_t *q, int a, int b)
{
    uint8_t cur = (uint8_t)(((a ? 1 : 0) << 1) | (b ? 1 : 0));
    int move = quad_table[(q->prev << 2) | cur];
    int per = q->per_detent;
    int step = 0;

    if (cur == q->prev) {
        return 0;
    }
    if (move == BAD) {
        q->invalid++;
        if ((per == 4 && q->acc == 0 && at_rest(q, q->prev)) || (per == 1 && q->dir == 0)) {
            /*
             * One state was missed just after a detent, where the dial
             * may have turned back, or with 1 per detent before any
             * direction is known. Wait for the next single transition
             * and count this one in its direction. With 1 per detent a
             * second such miss adds to the wait.
             */
            q->prev = cur;
            if (q->pend < 3) {
                q->pend++;
            }
            return 0;
        }
        /* One state was missed mid-detent. Assume the dial kept its last direction. */
        move = 2 * q->dir;
    } else {
        q->dir = (int8_t)move;
        if (q->pend) {
            move *= 1 + 2 * q->pend;
            q->pend = 0;
        }
    }
    q->prev = cur;
    q->acc = (int8_t)(q->acc + move);
    if (q->acc > ACC_LIMIT) {
        q->acc = ACC_LIMIT;
    } else if (q->acc < -ACC_LIMIT) {
        q->acc = -ACC_LIMIT;
    }

    if (per == 1) {
        /* Every state is a detent: report every transition, two for a missed state. */
        step = q->acc;
        q->acc = 0;
    } else if (at_rest(q, cur)) {
        /*
         * A rest state is a whole number of detents from the start, so acc
         * should be 0 or +-per here. Round it to the nearest detent, at
         * most one, and start again from 0: a missed state never shifts
         * the phase for good.
         */
        if (2 * q->acc >= per) {
            step = 1;
        } else if (2 * q->acc <= -per) {
            step = -1;
        }
        q->acc = 0;
        q->pend = 0;
    } else if (q->acc >= per) {
        /* A rest state was skipped. */
        q->acc = (int8_t)(q->acc - per);
        step = 1;
    } else if (q->acc <= -per) {
        q->acc = (int8_t)(q->acc + per);
        step = -1;
    }
    return q->reverse ? -step : step;
}
