/*
 * audio_rest_mid() in src/drv/audio.c: the ramp to the midpoint, its trace
 * lines, and the rest level after every way a sound can end. The test
 * builds the driver and the trace backend in, as test_audio_stop.c does.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

static uint8_t buf[800];
static const audio_note_t seq_rest[] = { { 440, 100 }, { 0, 100 } };

static void fill(void)
{
    size_t i;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 7u);
    }
}

/* Enters rest mode on `which` and plays the ramp to its end. */
static void enter_rest(int which)
{
    fresh(which);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(50000);
    tick(10);
    CHECK(!audio_busy());
    clear_logs();
}

/* The last PWM write of the record is the midpoint, and no write sets duty 0. */
static void check_rests_mid(const char *what)
{
    size_t n = strlen(pwm_log);

    if (n < 4 || strcmp(pwm_log + n - 4, "mid;") != 0 || strstr(pwm_log, "out 0 ")) {
        RC_FAIL("%s: PWM writes \"%s\"", what, pwm_log);
    }
}

TEST(ramp_values_follow_the_curve)
{
    size_t i;

    fresh(AUDIO_BACKEND_PWM);
    ramp_fill();
    CHECK_EQ(ramp[0], 0);
    CHECK_EQ(ramp[RAMP_LEN - 1], 128);
    for (i = 0; i < RAMP_LEN; i++) {
        double t = (double)i / (RAMP_LEN - 1);
        long want = (long)(128.0 * (3 * t * t - 2 * t * t * t) + 0.5);

        CHECK_EQ((long)ramp[i], want);
        if (i) {
            CHECK(ramp[i] >= ramp[i - 1]);
        }
    }
}

TEST(ramp_on_pwm_starts_at_zero_ignores_the_volume_and_rests_at_the_midpoint)
{
    size_t i;

    fresh(AUDIO_BACKEND_PWM);
    audio_set_volume(30);
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK(audio_busy());
    tick(10);
    /* PCM mode at the ramp's first sample, never at the midpoint first. */
    CHECK_STR(pwm_log, "at 0;");
    run_us(50000);
    CHECK(!pcm.active);
    CHECK_STR(pwm_log, "at 0;mid;");
    CHECK_EQ(pwm_count, RAMP_LEN);
    for (i = 0; i < RAMP_LEN && i < pwm_count; i++) {
        CHECK_EQ(pwm_samples[i], ramp[i]);
    }
    CHECK_EQ(rest_mid, 1);
}

TEST(ramp_on_trace_is_a_plain_play_at_vol_100)
{
    char want[256];

    fresh(AUDIO_BACKEND_TRACE);
    audio_set_volume(50);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(41000);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=22050 len=882 vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_end n=882 last=881 t_ms=1050\n",
             (unsigned)crc32(ramp, RAMP_LEN));
    CHECK_STR(out, want);
}

TEST(later_calls_do_nothing)
{
    enter_rest(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK(!audio_busy());
    tick(10);
    audio_trace_flush();
    CHECK_STR(out, "");
}

TEST(errors_leave_the_rest_level_alone)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(440, 1000), 0);
    CHECK_EQ(audio_rest_mid(), AUDIO_ERR_BUSY);
    tick(10);
    CHECK_EQ(audio_rest_mid(), AUDIO_ERR_BUSY);
    CHECK_EQ(rest_mid, 0);
    audio_stop();
    tick(10);
    CHECK_STR(pwm_log, "out 440 100;out 0 100;");
    CHECK_EQ(audio_rest_mid(), 0);
    backend = -1;
    rest_mid = 0;
    CHECK_EQ(audio_rest_mid(), AUDIO_ERR_NO_INIT);
}

TEST(without_the_call_silence_stays_at_duty_0)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(120000);
    CHECK_STR(pwm_log, "mid;out 0 0;");
    pwm_log[0] = '\0';
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    CHECK_STR(pwm_log, "out 440 100;out 0 100;");
}

TEST(rest_after_a_natural_pcm_end)
{
    fill();
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(120000);
    CHECK(!pcm.active);
    CHECK_STR(pwm_log, "mid;mid;");
}

TEST(rest_after_a_pcm_stop)
{
    fill();
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(20000);
    audio_stop();
    tick(10);
    check_rests_mid("pcm stop");
}

TEST(rest_after_a_queued_pcm_cut)
{
    fill();
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    CHECK_EQ(audio_pcm_queue(buf, sizeof(buf)), 0);
    tick(10);
    run_us(20000);
    audio_stop();
    tick(10);
    check_rests_mid("queued pcm stop");
}

TEST(rest_after_pcm_replaced_by_a_tone_that_ends)
{
    fill();
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(20000);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    CHECK(strstr(pwm_log, "out 440 100;") != 0);
    tick(40);
    check_rests_mid("pcm, then a tone");
}

TEST(a_tone_after_the_call_sounds_and_ends_at_the_rest_level)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    CHECK_STR(pwm_log, "out 440 100;");
    tick(40);
    CHECK_STR(pwm_log, "out 440 100;mid;");
}

TEST(rest_after_a_tone_stop)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(440, 3000), 0);
    tick(10);
    audio_stop();
    tick(10);
    CHECK_STR(pwm_log, "out 440 100;mid;");
}

TEST(rest_in_a_sequence_rest_and_after_its_stop)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play(seq_rest, 2, AUDIO_LOOP), 0);
    tick(10);
    tick(110);
    CHECK_STR(pwm_log, "out 440 100;mid;");
    audio_stop();
    tick(10);
    check_rests_mid("sequence stop");
}

TEST(a_tone_at_volume_0_rests_at_the_midpoint)
{
    enter_rest(AUDIO_BACKEND_PWM);
    audio_set_volume(0);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    check_rests_mid("tone at volume 0");
}

TEST(rest_after_a_pcm_replaced_by_pcm)
{
    fill();
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(20000);
    CHECK_EQ(audio_play_pcm(buf, 80, 8000, AUDIO_PRIORITY), 0);
    tick(10);
    run_us(20000);
    check_rests_mid("pcm replaced by pcm");
}

/*
 * After a ramp that ended early: the rest level is still duty 0, no ramp
 * is left, and a second call plays the ramp to its end.
 */
static void check_retry(const char *what)
{
    if (rest_mid || rest_ramp) {
        RC_FAIL("%s: rest_mid %d rest_ramp %d", what, rest_mid, rest_ramp);
    }
    CHECK(!audio_busy());
    clear_logs();
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK_EQ(rest_mid, 0);
    tick(10);
    run_us(50000);
    tick(10);
    if (!rest_mid || strcmp(pwm_log, "at 0;mid;") != 0) {
        RC_FAIL("%s: retry rest_mid %d PWM writes \"%s\"", what, rest_mid, pwm_log);
    }
}

TEST(a_call_during_the_ramp_does_nothing)
{
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK_EQ(rest_mid, 0);
    run_us(50000);
    tick(10);
    CHECK_EQ(rest_mid, 1);
    CHECK_STR(pwm_log, "at 0;mid;");
}

TEST(a_stop_before_the_ramp_starts_leaves_duty_0)
{
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    audio_stop();
    tick(10);
    /* The ramp never started: the pin stays at its initial duty 0. */
    CHECK_STR(pwm_log, "");
    check_retry("stop before the ramp");
}

TEST(a_stop_during_the_ramp_writes_duty_0)
{
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    audio_stop();
    CHECK_STR(pwm_log, "at 0;out 0 0;");
    tick(10);
    CHECK_STR(pwm_log, "at 0;out 0 0;out 0 0;");
    check_retry("stop during the ramp");
}

TEST(a_priority_pcm_cuts_the_playing_ramp)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    CHECK_EQ(audio_play_pcm(buf, 80, 8000, AUDIO_PRIORITY), 0);
    tick(10);
    run_us(20000);
    tick(10);
    /* The cut, the play, then its end at duty 0. */
    CHECK_STR(pwm_log, "at 0;out 0 0;out 0 0;mid;out 0 0;");
    check_retry("priority pcm during the ramp");
}

TEST(a_priority_pcm_replaces_the_pending_ramp)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK_EQ(audio_play_pcm(buf, 80, 8000, AUDIO_PRIORITY), 0);
    tick(10);
    run_us(20000);
    tick(10);
    CHECK_STR(pwm_log, "mid;out 0 0;");
    check_retry("priority pcm before the ramp");
}

TEST(a_tone_replaces_the_pending_ramp)
{
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    CHECK_STR(pwm_log, "out 440 100;out 0 100;");
    check_retry("tone before the ramp");
}

TEST(a_tone_cuts_the_playing_ramp)
{
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    CHECK_STR(pwm_log, "at 0;out 0 0;out 0 0;out 440 100;out 0 100;");
    check_retry("tone during the ramp");
}

TEST(a_trace_ramp_stopped_early_leaves_duty_0)
{
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    audio_stop();
    tick(10);
    CHECK_EQ(rest_mid, 0);
    CHECK_EQ(rest_ramp, 0);
    CHECK(!audio_busy());
}

TEST(trace_of_tones_unchanged_in_rest_mode)
{
    enter_rest(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    audio_trace_flush();
    CHECK_STR(out, "@RC stage=host ev=audio op=tone hz=440 vol=100 t_ms=1080\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1110\n");
}
