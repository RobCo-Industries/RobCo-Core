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

/*
 * A centred tone plays: the play is of kind tone, no pulse write, and the
 * sample writes from `from` on are the square of `hz` at full height for
 * one whole period at least.
 */
static void check_centred_tone(const char *what, size_t from, uint32_t hz)
{
    audio_square_t q;
    uint32_t half = (ROBCO_AUDIO_TONE_RATE * 128u + hz / 2u) / hz;
    size_t period = 2u * half / 256u + 1u;
    size_t i;

    if (!pcm.active || pcm_kind != KIND_TONE || strstr(pwm_log, "out ")) {
        RC_FAIL("%s: no tone play, PWM writes \"%s\"", what, pwm_log);
        return;
    }
    run_us((uint32_t)(period * 1000000u / ROBCO_AUDIO_TONE_RATE + 100u));
    if (pwm_count < from + period) {
        RC_FAIL("%s: %u sample writes, want %u", what, (unsigned)pwm_count,
                (unsigned)(from + period));
        return;
    }
    audio_square_start(&q, hz, ROBCO_AUDIO_TONE_RATE, audio_square_amp(ROBCO_AUDIO_TONE_PEAK, 100));
    for (i = 0; i < period; i++) {
        uint32_t want = audio_pcm_duty(audio_square_at(&q, (uint32_t)i), AUDIO_PCM_PWM_RANGE);

        if (pwm_samples[from + i] != want) {
            RC_FAIL("%s: sample %u is %u, want %u", what, (unsigned)i,
                    (unsigned)pwm_samples[from + i], (unsigned)want);
            return;
        }
    }
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

/* Runs the channel 3 IRQ `late_us` after its due time. */
static void irq_late(uint32_t late_us)
{
    fake_step(fake_due - fake_us + late_us);
    pcm_isr(IRQ_TIMER3, 0);
}

/*
 * The ramp with one IRQ late by `late` samples after `at` steps were
 * written. No write moves the duty by more than the ramp's largest step, the pin ends at duty 128, and the rest level
 * and busy answer change only then.
 */
static void ramp_late(uint32_t at, uint32_t late)
{
    uint32_t big = 0;
    uint32_t prev;
    unsigned guard = 0;
    size_t i;

    for (i = 1; i < RAMP_LEN; i++) {
        uint32_t d = audio_pcm_duty(ramp[i], AUDIO_PCM_PWM_RANGE) -
                     audio_pcm_duty(ramp[i - 1], AUDIO_PCM_PWM_RANGE);

        big = d > big ? d : big;
    }
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    while (pcm.active && pwm_count < at && guard++ < 10000) {
        irq_late(0);
    }
    if (pcm.active) {
        irq_late((uint32_t)(((uint64_t)late * 1000000u + RAMP_RATE - 1) / RAMP_RATE));
    }
    while (pcm.active && guard++ < 10000) {
        if (!audio_busy() || rest_mid || !rest_ramp) {
            RC_FAIL("at %u late %u: busy %d rest_mid %d during the ramp",
                    (unsigned)at, (unsigned)late, audio_busy(), rest_mid);
            return;
        }
        irq_late(0);
    }
    prev = audio_pcm_duty(ramp[0], AUDIO_PCM_PWM_RANGE);
    for (i = 0; i < pwm_count && i < sizeof(pwm_samples) / sizeof(pwm_samples[0]); i++) {
        uint32_t d = pwm_samples[i] > prev ? pwm_samples[i] - prev : prev - pwm_samples[i];

        if (d > big) {
            RC_FAIL("at %u late %u: write %u jumps by %u, ramp step max %u",
                    (unsigned)at, (unsigned)late, (unsigned)i, (unsigned)d, (unsigned)big);
            return;
        }
        prev = pwm_samples[i];
    }
    /* A late IRQ may drop steps, never the last one. */
    CHECK(pwm_count <= RAMP_LEN);
    CHECK_EQ(pcm.last, RAMP_LEN - 1);
    CHECK_EQ(prev, audio_pcm_duty(128, AUDIO_PCM_PWM_RANGE));
    CHECK_STR(pwm_log, "at 0;mid;");
    CHECK_EQ(rest_mid, 1);
    CHECK_EQ(rest_ramp, 0);
    tick(10);
    CHECK(!audio_busy());
}

TEST(a_late_irq_never_makes_the_ramp_jump)
{
    static const uint32_t lates[] = { 1, 10, 500, 881, 882, 5000 };
    static const uint32_t ats[] = { 1, 2, 300, 440, 800, 880, 881 };
    size_t i, j;

    for (i = 0; i < sizeof(lates) / sizeof(lates[0]); i++) {
        for (j = 0; j < sizeof(ats) / sizeof(ats[0]); j++) {
            ramp_late(ats[j], lates[i]);
        }
    }
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
    size_t from;

    fill();
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(buf, sizeof(buf), 8000, 0), 0);
    tick(10);
    run_us(20000);
    CHECK_EQ(audio_tone(440, 30), 0);
    from = pwm_count;
    tick(10);
    CHECK(strstr(pwm_log, "mid;mid;") != 0);
    check_centred_tone("pcm, then a tone", from, 440);
    tick(40);
    check_rests_mid("pcm, then a tone");
    CHECK(!pcm.active);
}

TEST(a_tone_after_the_call_sounds_and_ends_at_the_rest_level)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    CHECK_STR(pwm_log, "mid;");
    check_centred_tone("tone after the call", 0, 440);
    tick(40);
    CHECK_STR(pwm_log, "mid;mid;");
    CHECK(!pcm.active);
    CHECK(!audio_busy());
}

TEST(rest_after_a_tone_stop)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_tone(440, 3000), 0);
    tick(10);
    check_centred_tone("tone before its stop", 0, 440);
    audio_stop();
    /* The stop cuts the play before it returns: the pin is at 128 already. */
    CHECK_STR(pwm_log, "mid;mid;");
    CHECK(!pcm.active);
    tick(10);
    CHECK_STR(pwm_log, "mid;mid;mid;");
    CHECK(!audio_busy());
}

TEST(rest_in_a_sequence_rest_and_after_its_stop)
{
    enter_rest(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play(seq_rest, 2, AUDIO_LOOP), 0);
    tick(10);
    check_centred_tone("sequence note", 0, 440);
    tick(110);
    /* In the rest the play goes on and writes 128. */
    CHECK_STR(pwm_log, "mid;");
    CHECK(pcm.active && pcm_kind == KIND_TONE);
    {
        size_t from = pwm_count;
        size_t i;

        run_us(2000);
        CHECK(pwm_count > from + 50);
        for (i = from; i < pwm_count && i < 4096; i++) {
            CHECK_EQ(pwm_samples[i], 128);
        }
    }
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

TEST(rest_is_mid_turns_1_only_when_the_ramp_ends)
{
    static const int which[] = { AUDIO_BACKEND_PWM, AUDIO_BACKEND_TRACE };
    size_t i;

    for (i = 0; i < 2; i++) {
        fresh(which[i]);
        CHECK_EQ(audio_rest_is_mid(), 0);
        CHECK_EQ(audio_rest_mid(), 0);
        /* The ramp waits for its tick, then plays. */
        CHECK_EQ(audio_rest_is_mid(), 0);
        tick(10);
        CHECK_EQ(audio_rest_is_mid(), 0);
        run_us(20000);
        CHECK(audio_busy());
        CHECK_EQ(audio_rest_is_mid(), 0);
        run_us(30000);
        /* Not busy and at the midpoint from the same IRQ. */
        CHECK(!audio_busy());
        CHECK_EQ(audio_rest_is_mid(), 1);
        /* A later sound and its stop leave it. */
        CHECK_EQ(audio_tone(440, 30), 0);
        tick(10);
        CHECK_EQ(audio_rest_is_mid(), 1);
        audio_stop();
        tick(10);
        CHECK(!audio_busy());
        CHECK_EQ(audio_rest_is_mid(), 1);
    }
}

TEST(rest_is_mid_stays_0_after_a_ramp_that_ends_early)
{
    fill();
    /* A stop before the ramp starts. */
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    audio_stop();
    tick(10);
    CHECK(!audio_busy());
    CHECK_EQ(audio_rest_is_mid(), 0);
    /* A stop while it plays. */
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    audio_stop();
    tick(10);
    CHECK(!audio_busy());
    CHECK_EQ(audio_rest_is_mid(), 0);
    /* A priority play while it plays, then a second ramp that reaches its end. */
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(10000);
    CHECK_EQ(audio_play_pcm(buf, 80, 8000, AUDIO_PRIORITY), 0);
    tick(10);
    run_us(20000);
    tick(10);
    CHECK(!audio_busy());
    CHECK_EQ(audio_rest_is_mid(), 0);
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    run_us(50000);
    CHECK(!audio_busy());
    CHECK_EQ(audio_rest_is_mid(), 1);
    /* Before audio_init(). */
    fresh(AUDIO_BACKEND_PWM);
    backend = -1;
    CHECK_EQ(audio_rest_is_mid(), 0);
}
