#include <stdint.h>

#include <robco/input.h>

/*
 * Gray-code state table, indexed by (prev << 2) | cur with each state
 * (A << 1) | B. Clockwise runs 00 -> 01 -> 11 -> 10 -> 00. A change of
 * one bit is a step of +1 or -1. A change of both bits means a sample
 * was missed, so the direction is unknown: the step is rejected.
 */
#define BAD 2

static const int8_t quad_table[16] = {
    /* prev 00 */ 0, 1, -1, BAD,
    /* prev 01 */ -1, 0, BAD, 1,
    /* prev 10 */ 1, BAD, 0, -1,
    /* prev 11 */ BAD, -1, 1, 0,
};

void input_quad_init(input_quad_t *q, unsigned per_detent, int reverse, int a, int b)
{
    if (per_detent != 1 && per_detent != 2) {
        per_detent = 4;
    }
    q->per_detent = (uint8_t)per_detent;
    q->reverse = reverse ? 1 : 0;
    q->prev = (uint8_t)(((a ? 1 : 0) << 1) | (b ? 1 : 0));
    q->acc = 0;
    q->invalid = 0;
}

int input_quad_feed(input_quad_t *q, int a, int b)
{
    uint8_t cur = (uint8_t)(((a ? 1 : 0) << 1) | (b ? 1 : 0));
    int step = quad_table[(q->prev << 2) | cur];

    q->prev = cur;
    if (step == BAD) {
        q->invalid++;
        return 0;
    }
    if (q->reverse) {
        step = -step;
    }
    q->acc = (int8_t)(q->acc + step);
    if (q->acc >= q->per_detent) {
        q->acc = (int8_t)(q->acc - q->per_detent);
        return 1;
    }
    if (q->acc <= -q->per_detent) {
        q->acc = (int8_t)(q->acc + q->per_detent);
        return -1;
    }
    return 0;
}
