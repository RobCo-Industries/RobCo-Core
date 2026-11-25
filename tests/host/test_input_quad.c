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

/* Feeds Gray position pos (0..3 in cw[]) and returns the step. */
static int feed_pos(input_quad_t *q, int pos)
{
    int s = cw[((pos % 4) + 4) % 4];

    return input_quad_feed(q, (s >> 1) & 1, s & 1);
}

TEST(quad_every_transition)
{
    input_quad_t q;
    int from, to, dir;

    /* From each state to each state, with no direction known yet, then after +1 and -1. */
    for (dir = -1; dir <= 1; dir++) {
        for (from = 0; from < 4; from++) {
            for (to = 0; to < 4; to++) {
                int d = (to - from + 4) % 4;
                int before;

                /* Rest at position 2, far from the moves, so no rounding happens. */
                input_quad_init(&q, 4, 0, 0, 1);    /* cw[1] = 01 is the rest state */
                q.rest = (uint8_t)cw[(from + 2) % 4];
                q.prev = (uint8_t)cw[from];
                q.dir = (int8_t)dir;
                before = q.acc;
                if (q.rest == cw[to]) {
                    continue;
                }
                CHECK_EQ(feed_pos(&q, to), 0);
                if (d == 0) {
                    CHECK_EQ(q.acc, before);
                    CHECK_EQ(q.invalid, 0);
                } else if (d == 1 || d == 3) {
                    CHECK_EQ(q.acc, before + (d == 1 ? 1 : -1));
                    CHECK_EQ(q.dir, d == 1 ? 1 : -1);
                    CHECK_EQ(q.invalid, 0);
                } else {
                    /* Both lines changed: two steps the last way, or nothing. */
                    CHECK_EQ(q.acc, before + 2 * dir);
                    CHECK_EQ(q.dir, dir);
                    CHECK_EQ(q.invalid, 1);
                }
            }
        }
    }
}

TEST(quad_double_transition_before_any_direction)
{
    input_quad_t q;

    input_quad_init(&q, 4, 0, 0, 0);
    /* 00 -> 11 skips a state at a detent: counted, held for the next step. */
    CHECK_EQ(input_quad_feed(&q, 1, 1), 0);
    CHECK_EQ(q.invalid, 1);
    CHECK_EQ(q.acc, 0);
    CHECK_EQ(q.pend, 1);
    /* 11 -> 10 -> 00: the next step is clockwise, so one detent. */
    CHECK_EQ(run(&q, "2"), 0);
    CHECK_EQ(run(&q, "0"), 1);
    CHECK_EQ(q.acc, 0);
}

TEST(quad_missed_state_is_inferred)
{
    input_quad_t q;

    input_quad_init(&q, 4, 0, 0, 0);
    /* 00 01 [11 missed] 10 00: still one detent. */
    CHECK_EQ(run(&q, "12"), 0);
    CHECK_EQ(q.invalid, 1);
    CHECK_EQ(run(&q, "0"), 1);
    /* Counter-clockwise with a gap: 00 10 [11] 01 00. */
    CHECK_EQ(run(&q, "21"), 0);
    CHECK_EQ(run(&q, "0"), -1);
    /* The gap across the rest state: 00 01 11 10 [00] 01 11 10 00 is two detents. */
    CHECK_EQ(run(&q, "1321"), 1);
    CHECK_EQ(run(&q, "320"), 1);
    CHECK_EQ(q.acc, 0);
}

TEST(quad_turn_back_with_first_state_missed)
{
    input_quad_t q;

    /* Rest 11. One detent clockwise: 11 10 00 01 11, so dir is +. */
    input_quad_init(&q, 4, 0, 1, 1);
    CHECK_EQ(run(&q, "2013"), 1);
    /* Back counter-clockwise, 01 missed: 11 [01] 00 10 11 is still -1. */
    CHECK_EQ(run(&q, "0"), 0);
    CHECK_EQ(q.pend, 1);
    CHECK_EQ(run(&q, "2"), 0);
    CHECK_EQ(q.pend, 0);
    CHECK_EQ(run(&q, "3"), -1);
    /* And forward again with 10 missed: 11 [10] 00 01 11 is +1. */
    CHECK_EQ(run(&q, "013"), 1);
    CHECK_EQ(q.invalid, 2);
    CHECK_EQ(q.acc, 0);
}

TEST(quad_two_double_transitions_from_a_detent)
{
    input_quad_t q;

    /* Rest 11, dir + from one detent clockwise. */
    input_quad_init(&q, 4, 0, 1, 1);
    CHECK_EQ(run(&q, "2013"), 1);
    /*
     * 11 [10] 00 [01] 11: a fast turn that hides two states. The second
     * double transition takes the last direction: one detent clockwise.
     * A turn back hidden the same way would count +1 too: one step wrong.
     */
    CHECK_EQ(run(&q, "03"), 1);
    CHECK_EQ(q.pend, 0);
    CHECK_EQ(q.acc, 0);
    CHECK_EQ(run(&q, "1023"), -1);
}

TEST(quad_phase_recovers_at_rest)
{
    input_quad_t q;
    int pos = 0;

    input_quad_init(&q, 4, 0, 0, 0);
    /* A reversal hidden in a gap: +1, then 01 -> 10 guessed as +2 but it was -2. */
    CHECK_EQ(run(&q, "1"), 0);
    CHECK_EQ(run(&q, "2"), 0);      /* acc 3, really -1 */
    CHECK_EQ(run(&q, "0"), 1);      /* the rest state rounds 4: one wrong step at most */
    CHECK_EQ(q.acc, 0);
    /* After that every detent is exact again, both ways. */
    pos = 0;
    CHECK_EQ(turn(&q, &pos, 40), 10);
    CHECK_EQ(turn(&q, &pos, -40), -10);
    /* Two transitions forward and back at rest: nothing, the phase is right. */
    CHECK_EQ(turn(&q, &pos, 2), 0);
    CHECK_EQ(turn(&q, &pos, -2), 0);
}

TEST(quad_noise_between_rests_is_bounded)
{
    input_quad_t q;
    int i;

    input_quad_init(&q, 4, 0, 0, 0);
    CHECK_EQ(run(&q, "1"), 0);
    /* 01 <-> 10 over and over: both lines flip each sample. */
    for (i = 0; i < 100; i++) {
        CHECK(input_quad_feed(&q, i & 1, !(i & 1)) >= -1);
    }
    CHECK(q.acc <= 8 && q.acc >= -8);
    /* The next rest state ends it with one step at most. */
    {
        int r = run(&q, "0");

        CHECK(r >= -1 && r <= 1);
    }
    CHECK_EQ(q.acc, 0);
}

TEST(quad_two_per_detent_rests)
{
    input_quad_t q;

    /* Rest 11 and its complement 00. 11 10 00: one detent. */
    input_quad_init(&q, 2, 0, 1, 1);
    CHECK_EQ(run(&q, "20"), 1);
    CHECK_EQ(run(&q, "13"), 1);
    /* A missed state in the middle: 11 [10] 00 with the last direction +. */
    CHECK_EQ(run(&q, "0"), 1);
    CHECK_EQ(q.invalid, 1);
}

/* xorshift32, fixed seed: the same sequences every run. */
static uint32_t rnd_state = 0x1234567u;

static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

TEST(quad_long_sequences_match_a_reference)
{
    input_quad_t q;
    int run_i;

    /*
     * The reference is the true position. Runs of one direction, sampled
     * with gaps of at most one missed state and never two gaps in a row,
     * back to rest at the end of each run: the decoder must count exactly
     * the true detents. A gap may hide the first state of a run, also
     * when the run turns back.
     */
    for (run_i = 0; run_i < 200; run_i++) {
        int pos = 0, sum = 0, want, dir, n, k;

        input_quad_init(&q, 4, (int)(run_i & 1), 1, 1);
        rnd_state += (uint32_t)run_i;
        for (k = 0; k < 20; k++) {
            int start = pos, last = 1;

            dir = (rnd() & 1) ? 1 : -1;
            n = 1 + (int)(rnd() % 6);           /* detents this run */
            while (pos != start + dir * 4 * n) {
                int adv = (rnd() % 3 == 0 && last == 1) ? 2 : 1;

                if ((start + dir * 4 * n - pos) * dir < adv) {
                    adv = 1;
                }
                last = adv;
                pos += dir * adv;
                sum += feed_pos(&q, pos + 2);   /* cw[2] = 11 is the start */
            }
        }
        want = pos / 4;
        CHECK_EQ(sum, (run_i & 1) ? -want : want);
        CHECK_EQ(q.acc, 0);
    }
}

TEST(quad_bounce_at_rest_counts_once)
{
    input_quad_t q;

    input_quad_init(&q, 4, 0, 0, 0);
    /* Arrive at rest, then A chatters 00 <-> 10 a few times: one detent only. */
    CHECK_EQ(run(&q, "132"), 0);
    CHECK_EQ(run(&q, "0"), 1);
    CHECK_EQ(run(&q, "202020"), 0);
    CHECK_EQ(q.invalid, 0);
}

/* Moves the absolute position *pos by n transitions, feeding each. */
static int walk(input_quad_t *q, int *pos, int n)
{
    int sum = 0;

    for (; n > 0; n--) {
        sum += feed_pos(q, ++*pos);
    }
    for (; n < 0; n++) {
        sum += feed_pos(q, --*pos);
    }
    return sum;
}

TEST(quad_start_off_a_detent)
{
    input_quad_t q;
    int pos = 2, sum = 0, worst = 0, i;

    /*
     * The true detents are at Gray position 0 (00), but the layer starts
     * at position 2 (11), half a detent away. 11 becomes the rest state.
     */
    input_quad_init(&q, 4, 0, 1, 1);
    /* Two transitions to the true detent at 4: no step yet, one behind. */
    sum += walk(&q, &pos, 2);
    CHECK_EQ(sum, 0);
    /* Every detent still counts once, two transitions late. */
    sum += walk(&q, &pos, 40);
    CHECK_EQ(sum, 10);              /* truth 11 at position 44 */
    /* It never heals: the rest state stays at 11. */
    CHECK_EQ(q.rest, 3);
    /* Back to position 0: the total is right again in this direction. */
    sum += walk(&q, &pos, -44);
    CHECK_EQ(sum, 0);
    /* A walk both ways: at a true detent the total is off by one at most. */
    for (i = 0; i < 2000; i++) {
        sum += walk(&q, &pos, (rnd() & 1) ? 1 : -1);
        if ((pos & 3) == 0) {
            int err = sum - pos / 4;

            if (err < 0) {
                err = -err;
            }
            if (err > worst) {
                worst = err;
            }
        }
    }
    CHECK_EQ(worst, 1);
}

TEST(quad_initial_state)
{
    input_quad_t q;

    /* Starting at 11 (pull-ups at rest) does not count a step. */
    input_quad_init(&q, 4, 0, 1, 1);
    CHECK_EQ(input_quad_feed(&q, 1, 1), 0);
    CHECK_EQ(run(&q, "2013"), 1);
}

TEST(quad_rest_clears_any_history)
{
    input_quad_t q;
    int i;
    int k;

    /*
     * The target dial tests rely on this: a run with bad timing may count
     * wrong, but once the lines rest at 11 the next clean turn is exact.
     */
    input_quad_init(&q, 4, 0, 1, 1);
    for (i = 0; i < 500; i++) {
        int pos = 0;
        int n = (int)(rnd() % 12);

        /* Any states, jumps included, as a late host can make them. */
        for (k = 0; k < n; k++) {
            int s = (int)(rnd() & 3);

            (void)input_quad_feed(&q, (s >> 1) & 1, s & 1);
        }
        (void)input_quad_feed(&q, 1, 1);
        CHECK_EQ(q.acc, 0);
        CHECK_EQ(q.pend, 0);
        CHECK_EQ(q.prev, 3);
        /* cw[] starts at 00; position 2 is 11, the rest here. */
        pos = 2;
        CHECK_EQ(turn(&q, &pos, (i & 1) ? 16 : -16), (i & 1) ? 4 : -4);
    }
}
