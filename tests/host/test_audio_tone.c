/*
 * Tones once the rest level is the midpoint, in src/drv/audio.c: a
 * centred square on the sample path, its ends, rests, trace lines, late
 * IRQs, the busy answers and every call that reaches a tone. The test
 * builds the driver and the trace backend in, as test_audio_rest.c does.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

static uint8_t buf[800];
static uint8_t over[100];
static const audio_note_t seq_rest[] = { { 440, 30 }, { 0, 30 }, { 880, 30 } };
static const audio_note_t notes[] = { { 440, 1000 } };
static const audio_note_t two[] = { { 440, 20 }, { 3000, 20 } };
static const audio_voice_span_t spans[] = { { 800, 0, 0x10000000u, 0, 20 << 16, 0, 0 } };

/* Plays the ramp to its end, then puts the clock back to 1000 ms. */
static void enter_rest(int which)
{
    fresh(which);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(50000);
    tick(10);
    CHECK(rest_mid);
    CHECK(!audio_busy());
    clock_ms = 1000;
    acc_us = 0;
    last_us = fake_us;
    clear_logs();
}

/* Runs the channel 3 IRQ `late_us` after its due time. */
static void irq_late(uint32_t late_us)
{
    fake_step(fake_due - fake_us + late_us);
    pcm_isr(IRQ_TIMER3, 0);
}

static uint32_t amp_at(unsigned vol)
{
    return audio_square_amp(ROBCO_AUDIO_TONE_PEAK, vol);
}

static uint32_t dist(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

TEST(a_tone_writes_samples_and_no_pulse)
{
    audio_square_t q;
    size_t i;

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(3000, 30), 0);
    tick(10);
    CHECK(pcm.active && pcm_kind == KIND_TONE);
    run_us(10000);
    CHECK(strstr(pwm_log, "out ") == 0);
    CHECK_STR(pwm_log, "mid;");
    CHECK(pwm_count >= 370);
    audio_square_start(&q, 3000, ROBCO_AUDIO_TONE_RATE, amp_at(100));
    for (i = 0; i < 370; i++) {
        CHECK_EQ(pwm_samples[i], audio_square_at(&q, (uint32_t)i));
    }
    tick(40);
    CHECK_STR(pwm_log, "mid;mid;");
    CHECK(!audio_busy());
}

/* 20000 Hz is above half the grid: it plays at half the grid, not as silence. */
TEST(a_top_pitch_is_not_silent)
{
    size_t i;
    int moved = 0;

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(20000, 30), 0);
    tick(10);
    CHECK(pcm.active && pcm_kind == KIND_TONE);
    run_us(10000);
    CHECK(pwm_count >= 300);
    for (i = 1; i < 300; i++) {
        if (pwm_samples[i] != pwm_samples[i - 1]) {
            moved++;
        }
    }
    CHECK(moved > 100);
    CHECK_EQ(pwm_samples[0], 128 + amp_at(100));
    CHECK_EQ(pwm_samples[1], 128 - amp_at(100));
    tick(40);
}

/* The first write and the last one sit within the square's own step of 128. */
TEST(the_ends_step_no_more_than_the_square)
{
    static const unsigned vols[] = { 100, 20, 1 };
    size_t v;

    for (v = 0; v < 3; v++) {
        enter_rest(AUDIO_BACKEND_PWM);
        audio_set_volume(vols[v]);
        CHECK_EQ(audio_tone(440, 30), 0);
        tick(10);
        run_us(29000);
        tick(30);
        CHECK(!pcm.active);
        CHECK(pwm_count > 2 && pwm_count <= 4096);
        CHECK(dist(pwm_samples[0], 128) <= amp_at(vols[v]));
        CHECK(dist(pwm_samples[pwm_count - 1], 128) <= amp_at(vols[v]));
        /* Within the tone, no step is larger than twice the height. */
        {
            size_t i;

            for (i = 1; i < pwm_count; i++) {
                CHECK(dist(pwm_samples[i], pwm_samples[i - 1]) <= 2 * amp_at(vols[v]));
            }
        }
        /* The end returns to the midpoint, 128 in PCM mode. */
        CHECK_STR(pwm_log, "mid;mid;");
    }
}

TEST(a_sequence_rest_holds_128)
{
    size_t a;
    size_t i;

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play(seq_rest, 3, 0), 0);
    tick(10);
    run_us(29000);
    tick(1);
    /* 1040 ms: the rest. */
    CHECK_EQ(seq.hz, 0);
    a = pwm_count;
    run_us(29000);
    CHECK(pwm_count > a + 100);
    for (i = a; i < pwm_count; i++) {
        CHECK_EQ(pwm_samples[i], 128);
    }
    tick(1);
    CHECK_EQ(seq.hz, 880);
    /* The next note starts at an edge going up. */
    run_us(100);
    CHECK_EQ(pwm_samples[pwm_count - 1], 128 + amp_at(100));
    tick(40);
    CHECK(!pcm.active);
    CHECK(strstr(pwm_log, "out ") == 0);
}

/* Trace lines of a run, with or without the ramp before it. */
static void trace_run(int ramp, char *dst, size_t n)
{
    if (ramp) {
        enter_rest(AUDIO_BACKEND_TRACE);
    } else {
        fresh(AUDIO_BACKEND_TRACE);
    }
    CHECK_EQ(audio_play(seq_rest, 3, 0), 0);
    tick(10);
    run_us(5000);
    tick(30);
    tick(30);
    tick(30);
    CHECK_EQ(audio_play(notes, 1, 0), 0);
    tick(10);
    run_us(3000);
    audio_stop();
    tick(10);
    CHECK_EQ(audio_beep(), 0);
    tick(10);
    CHECK_EQ(audio_play_rtttl("t:d=4,o=5,b=200:c,p,e", AUDIO_PRIORITY), 0);
    tick(10);
    tick(300);
    tick(300);
    tick(300);
    CHECK(!audio_busy());
    audio_trace_flush();
    snprintf(dst, n, "%s", out);
}

TEST(trace_lines_match_the_run_without_the_ramp)
{
    static char with[sizeof(out)];
    static char without[sizeof(out)];

    trace_run(1, with, sizeof(with));
    trace_run(0, without, sizeof(without));
    CHECK(strstr(with, "op=tone hz=440") != 0);
    CHECK(strstr(with, "op=pcm") == 0);
    CHECK_STR(with, without);
}

TEST(beep_and_rtttl_play_centred)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_beep(), 0);
    tick(10);
    CHECK(pcm.active && pcm_kind == KIND_TONE);
    audio_stop();
    tick(10);
    CHECK_EQ(audio_play_rtttl("t:d=4,o=5,b=200:c,e", 0), 0);
    tick(10);
    CHECK(pcm.active && pcm_kind == KIND_TONE);
    CHECK(strstr(pwm_log, "out ") == 0);
}

/* A late IRQ skips samples; the square follows the grid by the skip rule. */
TEST(a_late_irq_skips_by_the_rule)
{
    static const uint32_t lates[] = { 0, 30, 1700, 1800, 20000 };
    size_t l;

    for (l = 0; l < sizeof(lates) / sizeof(lates[0]); l++) {
        audio_square_t q;
        uint32_t pos[64];
        size_t k;
        unsigned guard = 0;

        enter_rest(AUDIO_BACKEND_PWM);
        CHECK_EQ(audio_tone(3000, 1000), 0);
        tick(10);
        pos[0] = pcm.last_n;
        for (k = 1; k < 64 && guard++ < 1000; k++) {
            irq_late(k == 20 ? lates[l] : 0);
            pos[k] = pcm.last_n;
        }
        CHECK_EQ(pwm_count, 64);
        audio_square_start(&q, 3000, ROBCO_AUDIO_TONE_RATE, amp_at(100));
        for (k = 0; k < 64; k++) {
            if (pwm_samples[k] != audio_square_at(&q, pos[k])) {
                RC_FAIL("late %u: write %u at grid %u", (unsigned)lates[l], (unsigned)k,
                        (unsigned)pos[k]);
                break;
            }
        }
        /* The skip is real: the grid moved past the late time. */
        CHECK(pos[20] - pos[19] >= 1 + lates[l] * ROBCO_AUDIO_TONE_RATE / 1000000u);
        if (pos[20] - pos[19] - 1 > AUDIO_SQUARE_SKIP_MAX) {
            /* A long skip restarts at an edge going up: 3000 Hz is 6.25 samples high. */
            static const uint32_t fresh_start[] = { 255, 255, 255, 255, 255, 255, 64 };

            for (k = 0; k < 7; k++) {
                CHECK_EQ(pwm_samples[20 + k], fresh_start[k]);
            }
        }
    }
}

TEST(volume_slews_within_a_note)
{
    uint32_t top = 0;
    size_t a;
    size_t i;

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play(two, 2, 0), 0);
    tick(10);
    run_us(200);
    a = pwm_count;
    audio_set_volume(20);
    run_us(19000);
    /* No jump: the height falls by at most one gain step per sample. */
    CHECK(dist(pwm_samples[a], 128) >= amp_at(100) - 1);
    for (i = a + 1; i < pwm_count; i++) {
        CHECK(dist(pwm_samples[i], 128) <= dist(pwm_samples[a], 128));
    }
    /* Within 20 ms the note sits at the new height, before its next note. */
    for (i = pwm_count - 100; i < pwm_count; i++) {
        uint32_t d = dist(pwm_samples[i], 128);

        top = d > top ? d : top;
    }
    CHECK_EQ(top, amp_at(20));
    tick(20);
    run_us(100);
    CHECK_EQ(pwm_samples[pwm_count - 1], 128 + amp_at(20));
}

TEST(a_priority_start_cuts_to_128_and_prints_stop_first)
{
    char want[512];

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(440, 1000), 0);
    tick(10);
    run_us(1000);
    k_memset(buf, 128, sizeof(buf));
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), AUDIO_ERR_BUSY);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, AUDIO_PRIORITY), 0);
    /* Cut at once: the pin is at 128 before the call returns. */
    CHECK_STR(pwm_log, "mid;mid;");
    CHECK(!pcm.active);
    tick(10);
    CHECK(pcm.active && pcm_kind == KIND_SAMPLE);

    enter_rest(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_tone(440, 1000), 0);
    tick(10);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, AUDIO_PRIORITY), 0);
    tick(10);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=tone hz=440 vol=100 t_ms=1010\n"
             "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1020\n"
             "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=100 crc=%08x t_ms=1020\n",
             (unsigned)crc32(buf, sizeof(buf)));
    CHECK_STR(out, want);
}

TEST(a_stop_cuts_to_128_and_prints_one_stop)
{
    enter_rest(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_tone(440, 1000), 0);
    tick(10);
    run_us(1000);
    audio_stop();
    CHECK(!pcm.active);
    tick(10);
    CHECK(!audio_busy());
    audio_trace_flush();
    CHECK_STR(out, "@RC stage=host ev=audio op=tone hz=440 vol=100 t_ms=1010\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1021\n");
}

/* The trace backend runs the same play, so the sample count grows. */
TEST(trace_counts_samples_while_a_tone_sounds)
{
    uint32_t a;

    enter_rest(AUDIO_BACKEND_TRACE);
    a = audio_pcm_samples(0);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    run_us(10000);
    CHECK(audio_pcm_samples(0) > a + 300);
}

TEST(a_cancelled_ramp_keeps_pulse_tones)
{
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    audio_stop();
    tick(10);
    CHECK_EQ(rest_mid, 0);
    clear_logs();
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    CHECK_STR(pwm_log, "out 440 100;out 0 100;");
    CHECK_EQ(pwm_count, 0);
}

/* States after the ramp, each with a tone in it where one sounds. */
enum { T_REST, T_TONE_WAIT, T_TONE_ON, T_STOP_TONE, T_PCM_ON, T_VOICE_ON, T_COUNT };

static const char *const t_name[T_COUNT] = {
    "rest", "tone_wait", "tone_on", "stop_tone", "pcm_on", "voice_on",
};

static void t_enter(int st)
{
    enter_rest(AUDIO_BACKEND_TRACE);
    k_memset(buf, 128, sizeof(buf));
    k_memset(over, 140, sizeof(over));
    switch (st) {
    case T_TONE_WAIT:
        CHECK_EQ(audio_play(notes, 1, 0), 0);
        break;
    case T_TONE_ON:
        CHECK_EQ(audio_play(notes, 1, 0), 0);
        tick(10);
        CHECK(pcm.active && pcm_kind == KIND_TONE);
        break;
    case T_STOP_TONE:
        CHECK_EQ(audio_play(notes, 1, 0), 0);
        tick(10);
        audio_stop();
        break;
    case T_PCM_ON:
        CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
        tick(10);
        break;
    case T_VOICE_ON:
        CHECK_EQ(audio_play_voice(spans, 1, 8000, 0), 0);
        tick(10);
        break;
    default:
        break;
    }
    clear_logs();
}

#define B AUDIO_ERR_BUSY
#define S AUDIO_ERR_STATE

/* The tone rows of the guide's table, after the ramp. */
TEST(tone_rows_after_the_ramp)
{
    static const int st[] = { T_TONE_WAIT, T_TONE_ON };
    size_t i;

    for (i = 0; i < 2; i++) {
        t_enter(st[i]);
        CHECK_EQ(audio_busy(), 1);
        CHECK_EQ(audio_play(notes, 1, 0), B);
        CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), B);
        CHECK_EQ(audio_play_rtttl("t:d=4,o=5,b=200:c", 0), B);
        CHECK_EQ(audio_play_voice(spans, 1, 8000, 0), B);
        CHECK_EQ(audio_pcm_mix(over, sizeof(over)), S);
        CHECK_EQ(audio_pcm_queue(over, sizeof(over)), S);
        CHECK_EQ(audio_pcm_can_queue(), 0);
        CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, AUDIO_PRIORITY), 0);
    }
}

/* Once the rest level is the midpoint, audio_rest_mid() returns 0 and does nothing. */
TEST(rest_mid_does_nothing_in_every_state_after_the_ramp)
{
    int st;

    for (st = 0; st < T_COUNT; st++) {
        int busy;
        int active;
        int kind;

        t_enter(st);
        busy = audio_busy();
        active = pcm.active;
        kind = pcm_kind;
        if (audio_rest_mid() != 0) {
            RC_FAIL("%s: audio_rest_mid() is not 0", t_name[st]);
        }
        if (audio_busy() != busy || pcm.active != active || pcm_kind != kind ||
            rest_ramp || !rest_mid) {
            RC_FAIL("%s: audio_rest_mid() changed the state", t_name[st]);
        }
        audio_trace_flush();
        if (out[0]) {
            RC_FAIL("%s: audio_rest_mid() printed \"%s\"", t_name[st], out);
        }
    }
}

/* Gain across the notes of one sequence, and the time of its trace line. */

/* Runs the clock until the play outputs its next sample. */
static void next_out(void)
{
    uint32_t n = pcm.n;
    unsigned i;

    for (i = 0; i < 100000 && pcm.n == n; i++) {
        run_us(1);
    }
}

/* The gain the driver must hold at grid sample `p`, for a change from 10000 to 2000 at `join`. */
static uint32_t fall_at(uint32_t p, uint32_t join)
{
    uint32_t k = p - join + 1;

    return k >= 572 ? 2000 : 10000 - 14 * k;
}

/*
 * Two sounding notes, no rest, a change 5 ms before the note change. The
 * second note keeps the gain and its move: the height falls with no jump
 * and reaches the target 20 ms after the call, inside the second note.
 */
TEST(a_gain_change_goes_on_across_a_note_change)
{
    static const audio_note_t pair[] = { { 440, 20 }, { 880, 100 } };
    uint32_t join;
    uint32_t top = 0;
    uint32_t prev_g;
    int changed = 0;
    unsigned i;

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play(pair, 2, 0), 0);
    tick(10);
    run_us(15000);
    audio_set_gain(2000);
    join = gain_n + 1;
    prev_g = gain_cur;
    for (i = 0; i < 1200; i++) {
        uint32_t want;
        uint32_t s;

        if (!changed && fake_us - last_us >= 20000) {
            /* 1030 ms: the second note. No sample was skipped. */
            tick(0);
            CHECK_EQ(seq.hz, 880);
            changed = 1;
        }
        next_out();
        want = fall_at(pcm.last_n, join);
        if (gain_cur != want) {
            RC_FAIL("output %u, grid %u: gain %u, want %u", i, (unsigned)pcm.last_n,
                    (unsigned)gain_cur, (unsigned)want);
            return;
        }
        CHECK(prev_g - gain_cur <= 14);
        prev_g = gain_cur;
        s = pwm_samples[pwm_count - 1];
        CHECK(dist(s, 128) <= audio_square_amp_g(ROBCO_AUDIO_TONE_PEAK, gain_cur));
        if (pcm.last_n - join >= 750) {
            top = dist(s, 128) > top ? dist(s, 128) : top;
        }
    }
    CHECK(changed);
    /* The note change fell inside the move: 5 ms is 187 grid samples, the move takes 572. */
    CHECK_EQ(gain_cur, 2000);
    CHECK_EQ(top, audio_square_amp_g(ROBCO_AUDIO_TONE_PEAK, 2000));
}

/*
 * The same with a rest between the notes. The gain moves on through the
 * rest, which writes 128, and the note after it starts at the target.
 */
TEST(a_note_after_a_rest_starts_at_the_target)
{
    static const audio_note_t trio[] = { { 440, 20 }, { 0, 10 }, { 880, 100 } };
    uint32_t join;
    int phase = 0;
    unsigned i;

    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play(trio, 3, 0), 0);
    tick(10);
    run_us(15000);
    audio_set_gain(2000);
    join = gain_n + 1;
    for (i = 0; i < 1500; i++) {
        uint32_t s;

        if (phase == 0 && fake_us - last_us >= 20000) {
            tick(0);
            CHECK_EQ(seq.hz, 0);
            phase = 1;
        } else if (phase == 1 && fake_us - last_us >= 10000) {
            /* 1040 ms: 562 grid samples since the change, the move not done. */
            CHECK(gain_cur > 2000);
            tick(0);
            CHECK_EQ(seq.hz, 880);
            CHECK_EQ(gain_cur, 2000);
            phase = 2;
        }
        next_out();
        s = pwm_samples[pwm_count - 1];
        if (phase == 1) {
            CHECK_EQ(s, 128);
            CHECK_EQ(gain_cur, fall_at(pcm.last_n, join));
        } else if (phase == 2) {
            /* The first sample of the note is high, at the target. */
            CHECK_EQ(s, 128 + audio_square_amp_g(ROBCO_AUDIO_TONE_PEAK, 2000));
            CHECK_EQ(gain_cur, 2000);
            phase = 3;
        }
    }
    CHECK_EQ(phase, 3);
}

/* In a rest the driver prints no op=gain: the next note's line carries the gain. */
TEST(trace_a_change_in_a_rest_rides_on_the_next_note)
{
    static const audio_note_t trio[] = { { 440, 20 }, { 0, 10 }, { 880, 20 } };

    enter_rest(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play(trio, 3, 0), 0);
    tick(10);
    tick(20);
    run_us(5000);
    audio_set_gain(2000);
    tick(5);
    tick(20);
    audio_trace_flush();
    CHECK_STR(out, "@RC stage=host ev=audio op=tone hz=440 vol=100 t_ms=1010\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1030\n"
                   "@RC stage=host ev=audio op=tone hz=880 vol=20 t_ms=1040\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=20 t_ms=1060\n");
}
