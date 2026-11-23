#include <stdint.h>

#include <robco/audio.h>

#include "rc_test.h"

static uint8_t buf[1000];

/* Reference: floor(elapsed * rate / 10^6) with plain 64-bit math. */
static uint32_t ref_pos(uint32_t elapsed, uint32_t rate)
{
    return (uint32_t)((uint64_t)elapsed * rate / 1000000u);
}

TEST(check_limits)
{
    CHECK_EQ(audio_pcm_check(buf, 1, 8000, 0), 0);
    CHECK_EQ(audio_pcm_check(buf, 1, AUDIO_PCM_RATE_MIN, 0), 0);
    CHECK_EQ(audio_pcm_check(buf, 1, AUDIO_PCM_RATE_MAX, AUDIO_LOOP | AUDIO_PRIORITY), 0);
    CHECK_EQ(audio_pcm_check(buf, 1, AUDIO_PCM_RATE_MIN - 1, 0), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_pcm_check(buf, 1, AUDIO_PCM_RATE_MAX + 1, 0), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_pcm_check(buf, 1, 0, 0), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_pcm_check(rc_launder(NULL), 1, 8000, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pcm_check(buf, 0, 8000, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pcm_check(buf, (size_t)AUDIO_PCM_LEN_MAX + 1, 8000, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pcm_check(buf, 1, 8000, 4), AUDIO_ERR_ARG);
    /* A bad pointer wins over a bad rate. */
    CHECK_EQ(audio_pcm_check(rc_launder(NULL), 1, 1, 0), AUDIO_ERR_ARG);
}

TEST(positions_exact_early_late)
{
    static const uint32_t rates[] = { 4000, 8000, 11025, 22050 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        uint32_t r = rates[i];
        audio_pcm_t p;
        uint32_t pos = 99;
        uint32_t m;

        audio_pcm_start(&p, buf, sizeof buf, r, 0, 5000);
        CHECK_EQ(audio_pcm_poll(&p, 5000, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, 0);
        /* Same time again: nothing new. */
        CHECK_EQ(audio_pcm_poll(&p, 5000, &pos), AUDIO_PCM_NONE);
        for (m = 1; m < 50; m++) {
            uint32_t t = audio_pcm_next_us(&p) - 5000;

            /* t is the first microsecond of sample m. */
            CHECK_EQ(ref_pos(t, r), m);
            CHECK_EQ(ref_pos(t - 1, r), m - 1);
            /* Early by 1 us: nothing. Exact: sample m. */
            CHECK_EQ(audio_pcm_poll(&p, 5000 + t - 1, &pos), AUDIO_PCM_NONE);
            CHECK_EQ(audio_pcm_poll(&p, 5000 + t, &pos), AUDIO_PCM_SAMPLE);
            CHECK_EQ(pos, m);
        }
        CHECK_EQ(p.n, 50);
        /* Late: skips to the sample the clock says. */
        CHECK_EQ(audio_pcm_poll(&p, 5000 + 30000, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, ref_pos(30000, r));
        CHECK_EQ(p.n, 51);
        CHECK_EQ(p.last, pos);
    }
}

TEST(position_matches_reference_everywhere)
{
    static const uint32_t rates[] = { 4000, 4001, 8000, 11025, 16000, 22049, 22050 };
    unsigned i;

    for (i = 0; i < 7; i++) {
        audio_pcm_t p;
        uint32_t e;
        uint32_t pos;

        for (e = 0; e < (1u << 26); e += 99991) {
            audio_pcm_start(&p, buf, AUDIO_PCM_LEN_MAX, rates[i], 0, 0);
            CHECK_EQ(audio_pcm_poll(&p, e, &pos), AUDIO_PCM_SAMPLE);
            CHECK_EQ(pos, ref_pos(e, rates[i]));
        }
    }
}

TEST(end_after_last_sample)
{
    audio_pcm_t p;
    uint32_t pos = 0;

    /* 10 samples at 8000 Hz: 1250 us. */
    audio_pcm_start(&p, buf, 10, 8000, 0, 100);
    CHECK_EQ(audio_pcm_poll(&p, 100, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(audio_pcm_poll(&p, 100 + 9 * 125, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 9);
    CHECK_EQ(audio_pcm_next_us(&p), 100 + 1250);
    CHECK_EQ(audio_pcm_poll(&p, 100 + 1249, &pos), AUDIO_PCM_NONE);
    CHECK(p.active);
    CHECK_EQ(audio_pcm_poll(&p, 100 + 1250, &pos), AUDIO_PCM_END);
    CHECK_EQ(p.active, 0);
    CHECK_EQ(p.n, 2);
    CHECK_EQ(p.last, 9);
    CHECK_EQ(audio_pcm_poll(&p, 100 + 5000, &pos), AUDIO_PCM_NONE);
}

TEST(late_past_end_ends)
{
    audio_pcm_t p;
    uint32_t pos;

    audio_pcm_start(&p, buf, 10, 8000, 0, 0);
    CHECK_EQ(audio_pcm_poll(&p, 0, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(audio_pcm_poll(&p, 999999, &pos), AUDIO_PCM_END);
    CHECK_EQ(p.n, 1);
    CHECK_EQ(p.last, 0);
}

TEST(loop_wraps)
{
    audio_pcm_t p;
    uint32_t pos = 0;
    uint32_t t;

    audio_pcm_start(&p, buf, 10, 8000, AUDIO_LOOP, 0);
    for (t = 0; t < 125 * 35; t += 125) {
        CHECK_EQ(audio_pcm_poll(&p, t, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, (t / 125) % 10);
    }
    CHECK(p.active);
    CHECK_EQ(p.n, 35);
    /* Late by many loops. */
    CHECK_EQ(audio_pcm_poll(&p, 125 * 1003, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 3);
}

TEST(loop_runs_for_hours_without_drift)
{
    audio_pcm_t p;
    uint32_t pos = 0;
    uint64_t t;
    uint32_t len = 777;

    /* 11025 Hz: exact sample grid every 40 ms. Steps cross the 2^32 us wrap. */
    audio_pcm_start(&p, buf, len, 11025, AUDIO_LOOP, 0xF0000000u);
    for (t = 0; t < 3ull * 3600 * 1000000; t += 7777777) {
        CHECK_EQ(audio_pcm_poll(&p, (uint32_t)(0xF0000000u + t), &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, (uint32_t)(t * 11025 / 1000000 % len));
    }
}

TEST(long_one_shot_rebases)
{
    audio_pcm_t p;
    uint32_t pos = 0;

    /* 2^27 samples at 4000 Hz last 33554 s. Poll at 1000 s. */
    audio_pcm_start(&p, buf, 1u << 27, 4000, 0, 0);
    CHECK_EQ(audio_pcm_poll(&p, 1000000000u, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 4000000);
    CHECK_EQ(audio_pcm_poll(&p, 1000000250u, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 4000001);
    CHECK_EQ(audio_pcm_next_us(&p), 1000000500u);
}

TEST(volume_scale)
{
    CHECK_EQ(audio_pcm_scale(0, 100), 0);
    CHECK_EQ(audio_pcm_scale(128, 100), 128);
    CHECK_EQ(audio_pcm_scale(255, 100), 255);
    CHECK_EQ(audio_pcm_scale(0, 50), 64);
    CHECK_EQ(audio_pcm_scale(128, 50), 128);
    CHECK_EQ(audio_pcm_scale(255, 50), 191);
    CHECK_EQ(audio_pcm_scale(0, 0), 128);
    CHECK_EQ(audio_pcm_scale(128, 0), 128);
    CHECK_EQ(audio_pcm_scale(255, 0), 128);
    CHECK_EQ(audio_pcm_scale(0, 500), 0);
}

TEST(duty)
{
    CHECK_EQ(audio_pcm_duty(0, AUDIO_PCM_PWM_RANGE), 0);
    CHECK_EQ(audio_pcm_duty(128, AUDIO_PCM_PWM_RANGE), 128);
    CHECK_EQ(audio_pcm_duty(255, AUDIO_PCM_PWM_RANGE), 255);
    CHECK_EQ(audio_pcm_duty(128, 512), 256);
}
