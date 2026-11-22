#include <stdint.h>

#include <robco/audio.h>

#include "rc_test.h"

static const audio_note_t three[] = { { 440, 100 }, { 0, 50 }, { 880, 200 } };

TEST(empty_and_null)
{
    audio_seq_t s = { 0 };
    static const audio_note_t zeros[] = { { 440, 0 }, { 880, 0 } };
    uint32_t at;

    CHECK_EQ(audio_seq_start(&s, NULL, 3, 0, 0), 0);
    CHECK_EQ(s.active, 0);
    CHECK_EQ(audio_seq_start(&s, three, 0, 0, 0), 0);
    CHECK_EQ(audio_seq_start(&s, zeros, 2, AUDIO_LOOP, 0), 0);
    CHECK_EQ(s.hz, 0);
    CHECK_EQ(audio_seq_poll(&s, 1000, &at), 0);
}

TEST(steps_in_order)
{
    audio_seq_t s = { 0 };
    uint32_t at = 0;

    CHECK_EQ(audio_seq_start(&s, three, 3, 0, 1000), 1);
    CHECK_EQ(s.hz, 440);
    CHECK_EQ(audio_seq_poll(&s, 1099, &at), 0);
    CHECK_EQ(s.hz, 440);
    CHECK_EQ(audio_seq_poll(&s, 1100, &at), 1);
    CHECK_EQ(at, 1100);
    CHECK_EQ(s.hz, 0);
    CHECK(s.active);
    CHECK_EQ(audio_seq_poll(&s, 1150, &at), 1);
    CHECK_EQ(at, 1150);
    CHECK_EQ(s.hz, 880);
    CHECK_EQ(audio_seq_poll(&s, 1349, &at), 0);
    CHECK_EQ(audio_seq_poll(&s, 1350, &at), 1);
    CHECK_EQ(at, 1350);
    CHECK_EQ(s.active, 0);
    CHECK_EQ(s.hz, 0);
    CHECK_EQ(audio_seq_poll(&s, 5000, &at), 0);
}

TEST(big_jump_reports_each_change_at_its_due_time)
{
    audio_seq_t s = { 0 };
    uint32_t at = 0;

    audio_seq_start(&s, three, 3, 0, 0);
    CHECK_EQ(audio_seq_poll(&s, 10000, &at), 1);
    CHECK_EQ(at, 100);
    CHECK_EQ(audio_seq_poll(&s, 10000, &at), 1);
    CHECK_EQ(at, 150);
    CHECK_EQ(s.hz, 880);
    CHECK_EQ(audio_seq_poll(&s, 10000, &at), 1);
    CHECK_EQ(at, 350);
    CHECK_EQ(s.active, 0);
    CHECK_EQ(audio_seq_poll(&s, 10000, &at), 0);
}

TEST(zero_length_steps_are_skipped)
{
    static const audio_note_t z[] = { { 300, 0 }, { 400, 10 }, { 500, 0 }, { 600, 0 }, { 700, 10 } };
    audio_seq_t s = { 0 };
    uint32_t at;
    uint32_t step;

    audio_seq_start(&s, z, 5, 0, 0);
    CHECK_EQ(s.hz, 400);
    step = s.step;
    CHECK_EQ(audio_seq_poll(&s, 10, &at), 1);
    CHECK_EQ(s.hz, 700);
    CHECK_EQ(s.step, step + 1);
    CHECK_EQ(audio_seq_poll(&s, 20, &at), 1);
    CHECK_EQ(s.active, 0);
}

TEST(loop_repeats)
{
    static const audio_note_t l[] = { { 500, 10 }, { 0, 0 }, { 600, 20 } };
    audio_seq_t s = { 0 };
    uint32_t at;
    uint32_t t;
    int i;

    audio_seq_start(&s, l, 3, AUDIO_LOOP, 0);
    for (i = 0, t = 0; i < 5; i++) {
        t += 10;
        CHECK_EQ(audio_seq_poll(&s, t, &at), 1);
        CHECK_EQ(at, t);
        CHECK_EQ(s.hz, 600);
        t += 20;
        CHECK_EQ(audio_seq_poll(&s, t, &at), 1);
        CHECK_EQ(at, t);
        CHECK_EQ(s.hz, 500);
        CHECK(s.active);
    }
}

TEST(loop_far_behind_is_bounded)
{
    static const audio_note_t l[] = { { 500, 1 }, { 600, 2 } };
    audio_seq_t s = { 0 };
    uint32_t at = 0;
    uint32_t now = 3000000001u;
    int n = 0;

    audio_seq_start(&s, l, 2, AUDIO_LOOP, 0);
    while (audio_seq_poll(&s, now, &at)) {
        n++;
        CHECK(n < 10);
        if (n >= 10) {
            break;
        }
    }
    /* 3000000001 = 1000000000 loops of 3 ms, plus 1: inside 600 Hz. */
    CHECK_EQ(s.hz, 600);
    CHECK_EQ(at, 3000000001u);
    CHECK(s.active);
}

TEST(stop_and_restart)
{
    audio_seq_t s = { 0 };
    uint32_t at;
    uint32_t step;

    audio_seq_start(&s, three, 3, AUDIO_LOOP, 0);
    step = s.step;
    audio_seq_stop(&s);
    CHECK_EQ(s.active, 0);
    CHECK_EQ(s.hz, 0);
    CHECK(s.step != step);
    CHECK_EQ(audio_seq_poll(&s, 1000, &at), 0);
    step = s.step;
    audio_seq_stop(&s);
    CHECK_EQ(s.step, step);
    CHECK_EQ(audio_seq_start(&s, three, 3, 0, 2000), 1);
    CHECK_EQ(s.hz, 440);
}

TEST(replaced_mid_play)
{
    static const audio_note_t other[] = { { 1000, 30 } };
    audio_seq_t s = { 0 };
    uint32_t at = 0;

    audio_seq_start(&s, three, 3, 0, 0);
    CHECK_EQ(audio_seq_poll(&s, 120, &at), 1);
    CHECK_EQ(s.hz, 0);
    audio_seq_start(&s, other, 1, 0, 125);
    CHECK_EQ(s.hz, 1000);
    CHECK_EQ(audio_seq_poll(&s, 154, &at), 0);
    CHECK_EQ(audio_seq_poll(&s, 155, &at), 1);
    CHECK_EQ(at, 155);
    CHECK_EQ(s.active, 0);
}

TEST(wraps_at_2_32_ms)
{
    audio_seq_t s = { 0 };
    uint32_t at = 0;
    uint32_t t0 = 0xFFFFFFC0u;

    audio_seq_start(&s, three, 3, 0, t0);
    CHECK_EQ(audio_seq_poll(&s, t0 + 99, &at), 0);
    CHECK_EQ(audio_seq_poll(&s, t0 + 100, &at), 1);
    CHECK_EQ(at, (uint32_t)(t0 + 100));
    CHECK(at < t0);
    CHECK_EQ(audio_seq_poll(&s, 0x00000100u, &at), 1);
    CHECK_EQ(at, (uint32_t)(t0 + 150));
    CHECK_EQ(s.hz, 880);
    CHECK_EQ(audio_seq_poll(&s, 0x00000100u, &at), 0);
}

TEST(period_saturates)
{
    static const audio_note_t big[] = { { 440, UINT32_MAX }, { 880, 5 } };
    audio_seq_t s = { 0 };

    audio_seq_start(&s, big, 2, AUDIO_LOOP, 0);
    CHECK_EQ(s.period_ms, UINT32_MAX);
    CHECK_EQ(s.hz, 440);
}
