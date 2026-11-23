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

static uint8_t buf_b[1000];

TEST(chain_errors)
{
    audio_pcm_t p;

    audio_pcm_start(&p, buf, 10, 8000, 0, 0);
    CHECK_EQ(audio_pcm_chain(&p, buf_b, 5), 0);
    CHECK_EQ(audio_pcm_chain(&p, buf_b, 5), AUDIO_ERR_BUSY);
    p.active = 0;
    CHECK_EQ(audio_pcm_chain(&p, buf_b, 5), AUDIO_ERR_STATE);
    audio_pcm_start(&p, buf, 10, 8000, AUDIO_LOOP, 0);
    CHECK_EQ(audio_pcm_chain(&p, buf_b, 5), AUDIO_ERR_STATE);
    CHECK(p.next == NULL);
}

/* Global index of what the chain outputs: earlier buffers plus pos. */
TEST(switch_on_exact_boundary)
{
    static const uint32_t rates[] = { 4000, 8000, 11025, 22050 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        uint32_t r = rates[i];
        audio_pcm_t p;
        uint32_t pos = 99;
        uint32_t m;

        audio_pcm_start(&p, buf, 10, r, 0, 7000);
        CHECK_EQ(audio_pcm_chain(&p, buf_b, 7), 0);
        CHECK_EQ(audio_pcm_poll(&p, 7000, &pos), AUDIO_PCM_SAMPLE);
        for (m = 1; m < 17; m++) {
            uint32_t t = audio_pcm_next_us(&p) - 7000;

            /* The grid of one buffer of 17 samples. */
            CHECK_EQ(ref_pos(t, r), m);
            CHECK_EQ(ref_pos(t - 1, r), m - 1);
            CHECK_EQ(audio_pcm_poll(&p, 7000 + t - 1, &pos), AUDIO_PCM_NONE);
            CHECK_EQ(p.data, m <= 10 ? buf : buf_b);
            CHECK_EQ(audio_pcm_poll(&p, 7000 + t, &pos), AUDIO_PCM_SAMPLE);
            CHECK_EQ(pos, m < 10 ? m : m - 10);
            CHECK_EQ(p.data, m < 10 ? buf : buf_b);
            CHECK_EQ(p.switches, m < 10 ? 0 : 1);
        }
        CHECK_EQ(p.n, 17);
        CHECK_EQ(p.last, 6);
        CHECK_EQ(audio_pcm_next_us(&p) - 7000, (uint32_t)((17ull * 1000000 + r - 1) / r));
        CHECK_EQ(audio_pcm_poll(&p, audio_pcm_next_us(&p) - 1, &pos), AUDIO_PCM_NONE);
        CHECK_EQ(audio_pcm_poll(&p, audio_pcm_next_us(&p), &pos), AUDIO_PCM_END);
        CHECK_EQ(p.n, 17);
        CHECK_EQ(p.last, 6);
    }
}

TEST(late_poll_lands_in_next_buffer)
{
    static const uint32_t rates[] = { 4000, 8000, 11025, 22050 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        uint32_t r = rates[i];
        audio_pcm_t p;
        uint32_t pos = 0;
        uint32_t t;

        audio_pcm_start(&p, buf, 10, r, 0, 0);
        CHECK_EQ(audio_pcm_chain(&p, buf_b, 20), 0);
        CHECK_EQ(audio_pcm_poll(&p, 0, &pos), AUDIO_PCM_SAMPLE);
        /* Late from sample 0 to the middle of sample 15. */
        t = (uint32_t)((15ull * 1000000 + r - 1) / r) + 20;
        CHECK_EQ(ref_pos(t, r), 15);
        CHECK_EQ(audio_pcm_poll(&p, t, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, 5);
        CHECK(p.data == buf_b);
        CHECK_EQ(p.switches, 1);
        CHECK(p.next == NULL);
        CHECK_EQ(audio_pcm_next_us(&p), (uint32_t)((16ull * 1000000 + r - 1) / r));

        /* Over a whole short next buffer: no third buffer, so the play ends. */
        audio_pcm_start(&p, buf, 10, r, 0, 0);
        CHECK_EQ(audio_pcm_chain(&p, buf_b, 3), 0);
        CHECK_EQ(audio_pcm_poll(&p, 0, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(audio_pcm_poll(&p, (uint32_t)(12ull * 1000000 / r) + 1, &pos),
                 AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, 2);
        CHECK_EQ(audio_pcm_poll(&p, (uint32_t)(14ull * 1000000 / r) + 1, &pos),
                 AUDIO_PCM_END);
        CHECK_EQ(p.switches, 1);
        CHECK_EQ(p.n, 2);
        CHECK_EQ(p.last, 2);

        /* Over the end of A and all of B in one poll. */
        audio_pcm_start(&p, buf, 10, r, 0, 0);
        CHECK_EQ(audio_pcm_chain(&p, buf_b, 3), 0);
        CHECK_EQ(audio_pcm_poll(&p, 0, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(audio_pcm_poll(&p, (uint32_t)(14ull * 1000000 / r) + 1, &pos),
                 AUDIO_PCM_END);
        CHECK_EQ(p.switches, 1);
        CHECK_EQ(p.active, 0);
    }
}

TEST(length_one_buffers)
{
    static const uint32_t rates[] = { 4000, 8000, 11025, 22050 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        uint32_t r = rates[i];
        audio_pcm_t p;
        uint32_t pos = 9;
        uint32_t m;

        audio_pcm_start(&p, buf, 1, r, 0, 0);
        CHECK_EQ(audio_pcm_poll(&p, 0, &pos), AUDIO_PCM_SAMPLE);
        CHECK_EQ(pos, 0);
        for (m = 1; m < 200; m++) {
            uint32_t t;

            CHECK_EQ(audio_pcm_chain(&p, buf + m, 1), 0);
            t = audio_pcm_next_us(&p);
            CHECK_EQ(ref_pos(t, r), m);
            CHECK_EQ(ref_pos(t - 1, r), m - 1);
            CHECK_EQ(audio_pcm_poll(&p, t - 1, &pos), AUDIO_PCM_NONE);
            CHECK_EQ(audio_pcm_poll(&p, t, &pos), AUDIO_PCM_SAMPLE);
            CHECK_EQ(pos, 0);
            CHECK(p.data == buf + m);
            CHECK_EQ(p.switches, m);
        }
        CHECK_EQ(audio_pcm_poll(&p, audio_pcm_next_us(&p), &pos), AUDIO_PCM_END);
        CHECK_EQ(p.n, 200);
    }
}

/*
 * Many chunks of varied lengths over minutes, across rebases and the 2^32
 * us wrap: the chained index equals the index in one long buffer.
 * A chunk holds at least 100 samples, more than one 3331 us step.
 */
TEST(many_switches_do_not_drift)
{
    static const uint32_t rates[] = { 4000, 8000, 11025, 22050 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        uint32_t r = rates[i];
        uint32_t t0 = 0xF8000000u;
        audio_pcm_t p;
        uint64_t done = 0;      /* samples in buffers already left */
        uint32_t k = 0;
        uint32_t pos = 0;
        uint64_t t;

        audio_pcm_start(&p, buf, 100 + (k * 337) % 900, r, 0, t0);
        for (t = 0; t < 400ull * 1000000; t += 3331) {
            uint32_t was_len = p.len;

            if (!p.next) {
                k++;
                CHECK_EQ(audio_pcm_chain(&p, buf, 100 + (k * 337) % 900), 0);
            }
            CHECK_EQ(audio_pcm_poll(&p, (uint32_t)(t0 + t), &pos), AUDIO_PCM_SAMPLE);
            if (p.switches) {
                CHECK_EQ(p.switches, 1);
                done += was_len;
                p.switches = 0;
            }
            CHECK_EQ(done + pos, t * r / 1000000);
        }
    }
}

/* The position passes 2^26 us, the origin rebases, then the buffer ends. */
TEST(switch_right_after_rebase)
{
    audio_pcm_t p;
    uint32_t pos = 0;
    uint32_t end_us = 68000000u;

    /* 68 s at 8000 Hz. */
    audio_pcm_start(&p, buf, 8000u * 68, 8000, 0, 100);
    CHECK_EQ(audio_pcm_poll(&p, 100, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(audio_pcm_poll(&p, 100 + end_us - 125, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 8000u * 68 - 1);
    CHECK(p.origin_us != 100);
    CHECK_EQ(audio_pcm_chain(&p, buf_b, 3), 0);
    CHECK_EQ(audio_pcm_next_us(&p), 100 + end_us);
    CHECK_EQ(audio_pcm_poll(&p, 100 + end_us - 1, &pos), AUDIO_PCM_NONE);
    CHECK_EQ(audio_pcm_poll(&p, 100 + end_us, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 0);
    CHECK(p.data == buf_b);
    CHECK_EQ(audio_pcm_poll(&p, 100 + end_us + 250, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 2);
    CHECK_EQ(audio_pcm_poll(&p, 100 + end_us + 375, &pos), AUDIO_PCM_END);

    /* A switch, then a rebase while the origin lies in the old buffer. */
    audio_pcm_start(&p, buf, 10, 8000, 0, 0);
    CHECK_EQ(audio_pcm_chain(&p, buf_b, 8000u * 100), 0);
    CHECK_EQ(audio_pcm_poll(&p, 1250, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 0);
    CHECK_EQ((int32_t)p.origin_pos, -10);
    CHECK_EQ(audio_pcm_poll(&p, 80000000u, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 8000u * 80 - 10);
    CHECK_EQ(audio_pcm_poll(&p, 80000125u, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 8000u * 80 - 9);
    /* Ends after 10 + 800000 samples. */
    CHECK_EQ(audio_pcm_poll(&p, 100001249u, &pos), AUDIO_PCM_SAMPLE);
    CHECK_EQ(pos, 8000u * 100 - 1);
    CHECK_EQ(audio_pcm_poll(&p, 100001250u, &pos), AUDIO_PCM_END);
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
