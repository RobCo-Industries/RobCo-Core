/*
 * The centred square engine in src/lib/audio_square.c: exact samples
 * worked out by hand, a reference written apart from the library, the
 * mean, the height rule, the skip rule and every pitch.
 */
#include <stdint.h>

#include <robco/audio.h>

#include "rc_test.h"

#define RATE    37500u

/* The rule's half period, with its floor of one sample. */
static uint32_t half_of(uint32_t hz, uint32_t rate)
{
    uint32_t half = (rate * 128u + hz / 2u) / hz;

    return half < 256u ? 256u : half;
}

/*
 * The area from the note's start to unit x, in 64 bits: the wave is +1
 * on even half periods and -1 on odd ones, so every whole period adds 0.
 */
static int64_t ref_area(uint64_t half, uint64_t x)
{
    uint64_t n = x / half;
    uint64_t r = x - n * half;

    return (n % 2 == 0) ? (int64_t)r : (int64_t)(half - r);
}

/* Area of sample j of a note: the difference of two areas, no walk. */
static int32_t ref_acc(uint32_t half, uint32_t j)
{
    return (int32_t)(ref_area(half, 256ull * (j + 1u)) - ref_area(half, 256ull * j));
}

/* round_half_away(amp * acc / 256) + 128, written with floating point. */
static int ref_out(int32_t acc, int amp)
{
    double v = (double)amp * acc / 256.0;
    int r = v >= 0 ? (int)(v + 0.5) : -(int)(-v + 0.5);

    return 128 + r;
}

static void check_first(uint32_t hz, const uint8_t *want, size_t n)
{
    audio_square_t q;
    size_t i;

    audio_square_start(&q, hz, RATE, 127);
    for (i = 0; i < n; i++) {
        uint8_t s = audio_square_at(&q, (uint32_t)i);

        if (s != want[i]) {
            RC_FAIL("%u Hz sample %u: %u, want %u", (unsigned)hz, (unsigned)i, s, want[i]);
            return;
        }
    }
}

/* Worked by hand from the rule, see the guide. half 10909. */
TEST(hand_440)
{
    uint8_t want[86];
    size_t i;

    CHECK_EQ(half_of(440, RATE), 10909);
    for (i = 0; i < 42; i++) {
        want[i] = 255;          /* 42 * 256 = 10752 units high */
    }
    want[42] = 157;             /* 157 high, 99 low: 128 + round(127 * 58 / 256) */
    for (i = 43; i < 85; i++) {
        want[i] = 1;            /* 128 - round(127.5) rounds away to 127 */
    }
    want[85] = 197;             /* 58 low, 198 high: 128 + round(127 * 140 / 256) */
    check_first(440, want, sizeof(want));
}

/* half 1600, 6.25 samples: the edges fall at quarter samples. */
TEST(hand_3000)
{
    static const uint8_t want[] = {
        255, 255, 255, 255, 255, 255, 64,   /* 64 high, 192 low: -63.5 rounds to -64 */
        1, 1, 1, 1, 1, 128,                 /* 128 low, 128 high */
        255, 255, 255, 255, 255, 192,       /* 192 high, 64 low */
    };

    CHECK_EQ(half_of(3000, RATE), 1600);
    check_first(3000, want, sizeof(want));
}

/*
 * 20000 Hz is above half the grid, so half is 256, not 240: the note
 * plays at 18750 Hz. Each sample is one whole level, an edge at its end.
 */
TEST(hand_20000)
{
    static const uint8_t want[] = { 255, 1, 255, 1, 255, 1, 255, 1, 255, 1 };

    CHECK_EQ(half_of(20000, RATE), 256);
    check_first(20000, want, sizeof(want));
    check_first(18750, want, sizeof(want));
}

/* Many periods against the 64-bit reference, at every height. */
static void check_ref(uint32_t hz, uint32_t samples, int amp)
{
    audio_square_t q;
    uint32_t half = half_of(hz, RATE);
    uint32_t j;

    audio_square_start(&q, hz, RATE, (unsigned)amp);
    for (j = 0; j < samples; j++) {
        int32_t acc = audio_square_acc(&q, j);
        int32_t want = ref_acc(half, j);

        if (acc != want || audio_square_out(acc, amp) != ref_out(want, amp)) {
            RC_FAIL("%u Hz amp %d sample %u: acc %d, want %d", (unsigned)hz, amp,
                    (unsigned)j, (int)acc, (int)want);
            return;
        }
    }
}

TEST(reference_over_many_periods)
{
    static const uint32_t hzs[] = { 20, 440, 1000, 3000, 7919, 15000, 19000, 20000 };
    size_t i;

    for (i = 0; i < sizeof(hzs) / sizeof(hzs[0]); i++) {
        check_ref(hzs[i], 200000, 127);
        check_ref(hzs[i], 20000, 64);
        check_ref(hzs[i], 20000, 1);
    }
}

/* Over a whole number of periods that is also a whole number of samples. */
static void check_mean(uint32_t hz)
{
    audio_square_t q;
    uint32_t half = half_of(hz, RATE);
    uint32_t a = 2 * half;
    uint32_t b = 256;
    uint32_t n;
    int64_t acc_sum = 0;
    int64_t out_sum = 0;
    uint32_t j;

    while (b) {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    n = 2 * half / a;           /* lcm(2 * half, 256) / 256 samples */
    audio_square_start(&q, hz, RATE, 127);
    for (j = 0; j < n; j++) {
        int32_t acc = audio_square_acc(&q, j);

        acc_sum += acc;
        out_sum += audio_square_out(acc, 127);
    }
    CHECK_EQ(acc_sum, 0);
    /* The output's mean rounds to 128: rounding moves each sample less than a half step. */
    CHECK(out_sum * 2 >= (int64_t)n * 255 && out_sum * 2 <= (int64_t)n * 257);
}

TEST(mean_over_whole_cycles_is_128)
{
    check_mean(440);
    check_mean(3000);
    check_mean(20000);
    check_mean(1234);
    check_mean(19000);
}

TEST(height_follows_the_volume)
{
    audio_square_t q;

    CHECK_EQ(audio_square_amp(127, 100), 127);
    CHECK_EQ(audio_square_amp(127, 80), 102);    /* 10210 / 100 */
    CHECK_EQ(audio_square_amp(127, 20), 25);     /* 2590 / 100 */
    CHECK_EQ(audio_square_amp(127, 1), 1);       /* 177 / 100 */
    CHECK_EQ(audio_square_amp(127, 0), 0);
    CHECK_EQ(audio_square_amp(127, 250), 127);
    /* A whole high sample is 128 + amp, a whole low one 128 - amp. */
    audio_square_start(&q, 440, RATE, 102);
    CHECK_EQ(audio_square_at(&q, 0), 230);
    audio_square_start(&q, 440, RATE, 25);
    CHECK_EQ(audio_square_at(&q, 0), 153);
    audio_square_start(&q, 440, RATE, 1);
    CHECK_EQ(audio_square_at(&q, 0), 129);
    CHECK_EQ(audio_square_out(-256, 1), 127);
    CHECK_EQ(audio_square_out(-128, 1), 127);   /* -0.5 rounds away */
    CHECK_EQ(audio_square_out(127, 1), 128);
}

/* A skip of `k` samples after `at` samples, against single steps. */
static void check_skip(uint32_t hz, uint32_t at, uint32_t k)
{
    audio_square_t q;
    audio_square_t r;
    uint32_t j;

    audio_square_start(&q, hz, RATE, 127);
    audio_square_start(&r, hz, RATE, 127);
    for (j = 0; j < at; j++) {
        (void)audio_square_acc(&q, j);
    }
    if (k <= AUDIO_SQUARE_SKIP_MAX) {
        /* The walk keeps the phase: the same as asking for every sample. */
        int32_t want = ref_acc(half_of(hz, RATE), at + k);

        CHECK_EQ(audio_square_acc(&q, at + k), want);
        CHECK_EQ(audio_square_acc(&q, at + k + 1), ref_acc(half_of(hz, RATE), at + k + 1));
    } else {
        /* A restart: the sample is sample 0 of a new note. */
        CHECK_EQ(audio_square_acc(&q, at + k), audio_square_acc(&r, 0));
        CHECK_EQ(audio_square_acc(&q, at + k + 1), audio_square_acc(&r, 1));
    }
}

TEST(skip_rule)
{
    static const uint32_t hzs[] = { 440, 3000, 20000 };
    size_t i;

    for (i = 0; i < 3; i++) {
        check_skip(hzs[i], 7, 1);
        check_skip(hzs[i], 7, 64);
        check_skip(hzs[i], 7, 65);
        check_skip(hzs[i], 7, 10000);
    }
}

TEST(a_grid_sample_that_goes_back_restarts)
{
    audio_square_t q;

    audio_square_start(&q, 3000, RATE, 127);
    (void)audio_square_acc(&q, 100);
    (void)audio_square_acc(&q, 101);
    CHECK_EQ(audio_square_acc(&q, 50), 256);
}

/*
 * Two edges in one sample. audio_square_start() no longer reaches this:
 * half is at least 256. The walk still handles any half above 0, so this
 * test sets half 253 by hand, the value 19000 Hz had before the floor,
 * to keep that part of the walk checked.
 */
TEST(two_edges_in_one_sample_by_hand)
{
    audio_square_t q;
    uint32_t half = 253;
    uint32_t j;
    int found = 0;

    audio_square_start(&q, 19000, RATE, 127);
    CHECK_EQ(q.half, 256);
    q.half = half;
    q.left = half;
    for (j = 0; j < 2000; j++) {
        uint64_t lo = 256ull * j;
        uint64_t hi = lo + 256;
        /* Edges strictly inside the sample. */
        uint64_t first = lo / half + 1;
        uint64_t edges = 0;
        uint64_t e;

        for (e = first; e * half < hi; e++) {
            edges++;
        }
        CHECK_EQ(audio_square_acc(&q, j), ref_acc(half, j));
        if (edges == 2) {
            found++;
        }
    }
    CHECK(found > 0);
}

/* At most one edge per sample through the public path, at every pitch. */
static void check_samples_alternate(uint32_t hz, uint32_t rate)
{
    audio_square_t q;
    int64_t sum = 0;
    uint32_t j;

    audio_square_start(&q, hz, rate, 127);
    for (j = 0; j < 1000; j++) {
        uint8_t s = audio_square_at(&q, j);

        if (s != (j % 2 ? 1 : 255)) {
            RC_FAIL("%u Hz at %u sample %u: %u", (unsigned)hz, (unsigned)rate, (unsigned)j, s);
            return;
        }
        sum += s;
    }
    CHECK_EQ(sum, 1000 * 128);
}

TEST(a_pitch_above_half_the_grid_plays_half_the_grid)
{
    check_samples_alternate(18750, 18750);
    check_samples_alternate(12000, 18750);
    check_samples_alternate(9375, 18750);
    check_samples_alternate(20000, 37500);
    check_samples_alternate(18750, 37500);
}

TEST(a_pitch_just_under_half_the_grid_is_untouched)
{
    audio_square_t q;

    /* 9374 Hz: (18750 * 128 + 4687) / 9374 = 256, the floor is not needed. */
    CHECK_EQ((18750u * 128u + 4687u) / 9374u, 256);
    audio_square_start(&q, 9374, 18750, 127);
    CHECK_EQ(q.half, 256);
    /* 9300 Hz: 258, above the floor, taken as is. */
    audio_square_start(&q, 9300, 18750, 127);
    CHECK_EQ(q.half, (18750u * 128u + 4650u) / 9300u);
    CHECK_EQ(q.half, 258);
    audio_square_start(&q, 18000, RATE, 127);
    CHECK_EQ(q.half, 267);
    check_ref(18000, 20000, 127);
}

/* No pitch is silent: every one swings at least amp each way of 128. */
TEST(no_pitch_is_silent)
{
    static const uint32_t rates[] = { AUDIO_PCM_RATE_MIN, 18750, RATE };
    size_t r;

    for (r = 0; r < 3; r++) {
        uint32_t hz;

        for (hz = AUDIO_HZ_MIN; hz <= AUDIO_HZ_MAX; hz++) {
            audio_square_t q;
            uint32_t n = 2u * (rates[r] / hz) + 4u;
            uint32_t j;
            int lo = 255;
            int hi = 0;

            audio_square_start(&q, hz, rates[r], 64);
            for (j = 0; j < n; j++) {
                int s = audio_square_at(&q, j);

                lo = s < lo ? s : lo;
                hi = s > hi ? s : hi;
            }
            if (hi - lo < 64) {
                RC_FAIL("%u Hz at %u: swing %d", (unsigned)hz, (unsigned)rates[r], hi - lo);
                return;
            }
        }
    }
}

TEST(a_rest_writes_128)
{
    audio_square_t q;

    audio_square_start(&q, 0, RATE, 127);
    CHECK_EQ(audio_square_at(&q, 0), 128);
    CHECK_EQ(audio_square_at(&q, 99999), 128);
    audio_square_start(&q, 440, RATE, 0);
    CHECK_EQ(audio_square_at(&q, 0), 128);
}

/* Every pitch in coarse steps, at both ends of the grid range. */
TEST(every_pitch_stays_in_range)
{
    static const uint32_t rates[] = { AUDIO_PCM_RATE_MIN, 18750, RATE };
    size_t r;

    for (r = 0; r < 3; r++) {
        uint32_t hz;

        for (hz = AUDIO_HZ_MIN; hz <= AUDIO_HZ_MAX; hz += hz < 1000 ? 7 : 97) {
            audio_square_t q;
            uint32_t j;

            audio_square_start(&q, hz, rates[r], 127);
            if (q.half == 0) {
                RC_FAIL("%u Hz at %u: half 0", (unsigned)hz, (unsigned)rates[r]);
                return;
            }
            for (j = 0; j < 400; j++) {
                int32_t acc = audio_square_acc(&q, j * 3u);

                if (acc < -256 || acc > 256) {
                    RC_FAIL("%u Hz at %u: acc %d", (unsigned)hz, (unsigned)rates[r], (int)acc);
                    return;
                }
                /* The rule itself keeps the byte within 1 to 255, with no clamp. */
                if (audio_square_out(acc, 127) != ref_out(acc, 127)) {
                    RC_FAIL("%u Hz: out of range", (unsigned)hz);
                    return;
                }
            }
        }
    }
}
