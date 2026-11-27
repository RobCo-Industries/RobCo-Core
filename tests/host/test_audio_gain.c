/*
 * The gain of the audio driver: the rules of src/lib/audio_gain.c against
 * the volume rules, the slew, and in src/drv/audio.c the smooth change in
 * a play and the trace lines. The test builds the driver and the trace
 * backend in, as test_audio_tone.c does.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

#include <stdio.h>

static uint8_t buf[800];

static uint32_t dist(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

TEST(sample_rule_equals_the_volume_rule)
{
    unsigned v;
    unsigned s;

    for (v = 0; v <= 100; v++) {
        for (s = 0; s < 256; s++) {
            if (audio_pcm_scale_g((uint8_t)s, v * 100) != audio_pcm_scale((uint8_t)s, v)) {
                RC_FAIL("sample %u at volume %u", s, v);
                return;
            }
        }
    }
}

TEST(voice_rule_equals_the_volume_rule)
{
    static const int32_t qs[] = { -32512, -32511, -12800, -129, -128, -127, -1, 0, 1, 127,
                                  128, 129, 12800, 32511, 32512, 65280 };
    unsigned v;
    unsigned m;
    size_t i;

    for (v = 0; v <= 100; v++) {
        for (m = 0; m < 256; m++) {
            for (i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
                if (audio_voice_out_g(qs[i], (uint8_t)m, v * 100) !=
                    audio_voice_out(qs[i], (uint8_t)m, v)) {
                    RC_FAIL("q %d mix %u at volume %u", (int)qs[i], m, v);
                    return;
                }
            }
        }
    }
    /* Every fine level of one step's range, alone. */
    for (v = 0; v <= 100; v++) {
        int32_t q;

        for (q = -32768; q <= 32768; q++) {
            if (audio_voice_out_g(q, 128, v * 100) != audio_voice_out(q, 128, v)) {
                RC_FAIL("q %d at volume %u", (int)q, v);
                return;
            }
        }
    }
}

TEST(square_and_pulse_rules_equal_the_volume_rule)
{
    unsigned v;
    unsigned peak;

    for (v = 0; v <= 100; v++) {
        for (peak = 0; peak <= 127; peak++) {
            CHECK_EQ(audio_square_amp_g(peak, v * 100), audio_square_amp(peak, v));
        }
        CHECK_EQ(audio_gain_pulse(v * 100), v);
    }
}

TEST(full_gain_returns_the_input)
{
    unsigned s;
    int32_t q;

    for (s = 0; s < 256; s++) {
        CHECK_EQ(audio_pcm_scale_g((uint8_t)s, AUDIO_GAIN_FULL), s);
        CHECK_EQ(audio_pcm_scale_g((uint8_t)s, 20000), s);
    }
    /* A voice on whole steps comes back as its byte. */
    for (q = -128 * 256; q <= 127 * 256; q += 256) {
        CHECK_EQ(audio_voice_out_g(q, 128, AUDIO_GAIN_FULL), 128 + q / 256);
    }
    CHECK_EQ(audio_square_amp_g(127, AUDIO_GAIN_FULL), 127);
    CHECK_EQ(audio_gain_pulse(AUDIO_GAIN_FULL), 100);
}

TEST(no_output_leaves_the_byte_range)
{
    static const int32_t qs[] = { -98000, -65280, -32768, 0, 32767, 65280, 98000 };
    uint32_t g;
    unsigned s;
    unsigned m;
    size_t i;

    for (g = 0; g <= AUDIO_GAIN_FULL; g += 7) {
        for (s = 0; s < 256; s++) {
            int d = (int)audio_pcm_scale_g((uint8_t)s, g) - 128;
            int in = (int)s - 128;

            /* Never further from the midpoint than the input, and on its side. */
            if ((in >= 0 && (d < 0 || d > in)) || (in < 0 && (d > 0 || d < in))) {
                RC_FAIL("sample %u at gain %u gives %d", s, (unsigned)g, d + 128);
                return;
            }
        }
        for (m = 0; m < 256; m += 17) {
            for (i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
                /* The byte type holds the result only when the rule clamps it. */
                int32_t x = (qs[i] + ((int32_t)m - 128) * 256) * (int32_t)g;
                int32_t r = x >= 0 ? (x + 1280000) / 2560000 : -((1280000 - x) / 2560000);
                int32_t want = r + 128 < 0 ? 0 : r + 128 > 255 ? 255 : r + 128;

                if (audio_voice_out_g(qs[i], (uint8_t)m, g) != want) {
                    RC_FAIL("voice q %d mix %u gain %u", (int)qs[i], m, (unsigned)g);
                    return;
                }
            }
        }
        CHECK(audio_square_amp_g(127, g) <= 127);
    }
}

/* From each start, toward each target: no step above `step`, no overshoot, in time. */
static void check_slew(uint32_t rate)
{
    static const uint32_t ends[] = { 0, 1, 99, 5000, 9999, 10000 };
    uint32_t step = audio_gain_step(rate);
    size_t a;
    size_t b;

    CHECK_EQ(step, (500000u + rate - 1) / rate);
    for (a = 0; a < sizeof(ends) / sizeof(ends[0]); a++) {
        for (b = 0; b < sizeof(ends) / sizeof(ends[0]); b++) {
            uint32_t cur = ends[a];
            uint32_t to = ends[b];
            uint32_t n = 0;

            while (cur != to) {
                uint32_t next = audio_gain_slew(cur, to, step);

                if (dist(next, cur) > step || dist(next, to) >= dist(cur, to) ||
                    (cur < to ? next > to : next < to)) {
                    RC_FAIL("rate %u: %u to %u steps %u to %u", (unsigned)rate,
                            (unsigned)ends[a], (unsigned)to, (unsigned)cur, (unsigned)next);
                    return;
                }
                cur = next;
                n++;
            }
            if (n > rate / 50) {
                RC_FAIL("rate %u: %u to %u took %u samples", (unsigned)rate,
                        (unsigned)ends[a], (unsigned)to, (unsigned)n);
            }
        }
    }
}

TEST(slew_at_8000_and_37500)
{
    CHECK_EQ(audio_gain_step(8000), 63);
    CHECK_EQ(audio_gain_step(37500), 14);
    check_slew(8000);
    check_slew(37500);
}

/* The driver: a change in a sample play moves the bytes by the slew, no jump. */
TEST(a_change_in_a_sample_play_slews)
{
    size_t a;
    size_t i;

    k_memset(buf, 255, sizeof(buf));
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(1000);
    a = pwm_count;
    CHECK(a > 0);
    CHECK_EQ(pwm_samples[a - 1], 255);
    audio_set_volume(20);
    run_us(30000);
    /* One gain step per sample at 8000 Hz: 63 units, under one byte step. */
    for (i = a; i < pwm_count; i++) {
        uint32_t g = i - a + 1 >= 127 ? 2000 : 10000 - 63 * (uint32_t)(i - a + 1);

        if (pwm_samples[i] != audio_pcm_scale_g(255, g)) {
            RC_FAIL("sample %u: %u, want %u", (unsigned)i, (unsigned)pwm_samples[i],
                    (unsigned)audio_pcm_scale_g(255, g));
            return;
        }
        CHECK(dist(pwm_samples[i], pwm_samples[i - 1]) <= 1);
    }
    /* At 8000 Hz the swing of 8000 units ends within 160 samples, 20 ms. */
    CHECK_EQ(pwm_samples[a + 159], audio_pcm_scale(255, 20));
}

/* A play starts at the target: nothing fades in. */
TEST(a_play_starts_at_the_target)
{
    k_memset(buf, 255, sizeof(buf));
    fresh(AUDIO_BACKEND_PWM);
    audio_set_volume(20);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(500);
    CHECK(pwm_count > 0);
    CHECK_EQ(pwm_samples[0], audio_pcm_scale(255, 20));
}

TEST(trace_one_gain_line_per_change_while_playing)
{
    char want[512];

    k_memset(buf, 200, sizeof(buf));
    fresh(AUDIO_BACKEND_TRACE);
    /* Nothing plays: no line. */
    audio_set_gain(5000);
    audio_set_gain(10000);
    audio_trace_flush();
    CHECK_STR(out, "");
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    /* The start waits for its tick: its start line carries the gain. */
    audio_set_volume(90);
    tick(10);
    audio_set_volume(50);
    audio_set_volume(50);
    audio_set_gain(1234);
    audio_set_gain(99999);
    run_us(100000 + 1000);
    tick(10);
    audio_set_gain(4000);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=90 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=gain gain=5000 t_ms=1010\n"
             "@RC stage=host ev=audio op=gain gain=1234 t_ms=1010\n"
             "@RC stage=host ev=audio op=gain gain=10000 t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_end n=800 last=799 t_ms=1110\n",
             (unsigned)crc32(buf, sizeof(buf)));
    CHECK_STR(out, want);
}

TEST(trace_start_lines_end_with_gain_off_a_whole_percent)
{
    static const audio_note_t one[] = { { 440, 20 } };
    char want[512];

    k_memset(buf, 200, sizeof(buf));
    fresh(AUDIO_BACKEND_TRACE);
    audio_set_gain(1234);
    CHECK_EQ(audio_play_pcm(buf, 80, 8000, AUDIO_LOOP), 0);
    tick(10);
    audio_stop();
    tick(10);
    audio_set_gain(1200);
    CHECK_EQ(audio_play(one, 1, 0), 0);
    tick(10);
    tick(30);
    audio_set_gain(1);
    CHECK_EQ(audio_play(one, 1, 0), 0);
    tick(10);
    tick(30);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=80 vol=12 crc=%08x t_ms=1010 loop=1 gain=1234\n"
             "@RC stage=host ev=audio op=stop hz=0 vol=12 t_ms=1020\n"
             "@RC stage=host ev=audio op=tone hz=440 vol=12 t_ms=1030\n"
             "@RC stage=host ev=audio op=stop hz=0 vol=12 t_ms=1050\n"
             "@RC stage=host ev=audio op=tone hz=440 vol=0 t_ms=1070 gain=1\n"
             "@RC stage=host ev=audio op=stop hz=0 vol=0 t_ms=1090\n",
             (unsigned)crc32(buf, 80));
    CHECK_STR(out, want);
}
