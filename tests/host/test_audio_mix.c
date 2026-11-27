/*
 * audio_pcm_mix() in src/drv/audio.c: one overlay added to the PCM that
 * plays, clamped, before the volume. Also audio_play_pcm() and
 * audio_pcm_mix() called from a tick callback. The test builds the driver
 * and the trace backend in, as test_audio_stop.c does.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

static uint8_t play[800];       /* 100 ms at 8000 Hz */
static uint8_t over[200];       /* 25 ms at 8000 Hz */
static uint8_t longer[2000];    /* 250 ms at 8000 Hz */

static void fill(void)
{
    size_t i;

    for (i = 0; i < sizeof(play); i++) {
        play[i] = (uint8_t)(100 + i % 50);
    }
    for (i = 0; i < sizeof(over); i++) {
        over[i] = (uint8_t)(120 + i % 20);
    }
    for (i = 0; i < sizeof(longer); i++) {
        longer[i] = (uint8_t)(i * 13u + 1u);
    }
}

static uint32_t clamp(int v)
{
    return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint32_t crc_of(const uint8_t *d, size_t n)
{
    return crc32(d, n);
}

TEST(errors)
{
    fill();
    fresh(AUDIO_BACKEND_TRACE);
    /* No play. */
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
    CHECK_EQ(audio_pcm_mix(rc_launder(NULL), 10), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pcm_mix(over, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pcm_mix(over, (size_t)AUDIO_PCM_LEN_MAX + 1), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    /* A tone is not a PCM play. */
    CHECK_EQ(audio_tone(440, 100), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
    tick(10);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
    /* A play with a stop that waits for the tick. */
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, AUDIO_PRIORITY), 0);
    tick(10);
    audio_stop();
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
    tick(10);
    audio_trace_flush();
    CHECK(strstr(out, "op=pcm_mix") == 0);
    backend = -1;
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_NO_INIT);
}

TEST(mix_over_a_play_adds_from_the_next_sample)
{
    char want[512];
    size_t i;
    size_t at;

    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    run_us(10000);
    at = pwm_count;
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 1);
    /* One overlay at a time. */
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_BUSY);
    run_us(100000);
    CHECK(!pcm.active);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    CHECK_EQ(pwm_count, sizeof(play));
    for (i = 0; i < pwm_count; i++) {
        int m = i >= at && i < at + sizeof(over) ? over[i - at] - 128 : 0;

        if (pwm_samples[i] != clamp(play[i] + m)) {
            RC_FAIL("sample %u: %u, want %u", (unsigned)i, (unsigned)pwm_samples[i],
                    (unsigned)clamp(play[i] + m));
        }
    }

    /* The trace: the mix line at its first sample, 10 ms in. */
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    run_us(10000);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    run_us(100000);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_mix len=200 crc=%08x t_ms=1020\n"
             "@RC stage=host ev=audio op=pcm_end n=800 last=799 t_ms=1110\n",
             (unsigned)crc_of(play, sizeof(play)), (unsigned)crc_of(over, sizeof(over)));
    CHECK_STR(out, want);
}

TEST(a_second_mix_after_the_first_ends)
{
    fill();
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_BUSY);
    run_us(30000);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 1);
}

TEST(a_mix_longer_than_the_play_ends_with_it)
{
    char want[512];

    fill();
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    run_us(50000);
    CHECK_EQ(audio_pcm_mix(longer, sizeof(longer)), 0);
    run_us(60000);
    CHECK(!pcm.active);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    CHECK(!audio_busy());
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_mix len=2000 crc=%08x t_ms=1060\n"
             "@RC stage=host ev=audio op=pcm_end n=800 last=799 t_ms=1110\n",
             (unsigned)crc_of(play, sizeof(play)), (unsigned)crc_of(longer, sizeof(longer)));
    CHECK_STR(out, want);
    /* The next play starts with no overlay. */
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
}

TEST(a_cut_drops_the_mix)
{
    int how;
    size_t n0;

    fill();
    for (how = 0; how < 3; how++) {
        fresh(AUDIO_BACKEND_PWM);
        CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
        tick(10);
        run_us(5000);
        CHECK_EQ(audio_pcm_mix(longer, sizeof(longer)), 0);
        run_us(5000);
        if (how == 0) {
            audio_stop();
        } else if (how == 1) {
            CHECK_EQ(audio_tone(440, 30), 0);
        } else {
            CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, AUDIO_PRIORITY), 0);
        }
        /* Dropped before the call returns, like the play. */
        CHECK_EQ(audio_pcm_mix_busy(), 0);
        n0 = pwm_count;
        tick(10);
        run_us(1000);
        if (how == 2) {
            size_t k;

            /* The new play has no overlay: its samples are its own. */
            CHECK(pwm_count > n0);
            for (k = 0; n0 + k < pwm_count; k++) {
                CHECK_EQ(pwm_samples[n0 + k], play[k]);
            }
        }
        CHECK_EQ(audio_pcm_mix_busy(), 0);
    }
}

TEST(a_play_that_waits_for_its_tick_takes_a_mix)
{
    char want[512];
    size_t i;

    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 1);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_BUSY);
    tick(10);
    run_us(110000);
    CHECK_EQ(pwm_count, sizeof(play));
    for (i = 0; i < pwm_count; i++) {
        int m = i < sizeof(over) ? over[i] - 128 : 0;

        CHECK_EQ(pwm_samples[i], clamp(play[i] + m));
    }

    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    tick(10);
    run_us(110000);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_mix len=200 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_end n=800 last=799 t_ms=1110\n",
             (unsigned)crc_of(play, sizeof(play)), (unsigned)crc_of(over, sizeof(over)));
    CHECK_STR(out, want);

    /* A replacement before the tick drops the waiting mix. */
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, AUDIO_PRIORITY), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    tick(10);
    audio_trace_flush();
    CHECK(strstr(out, "op=pcm_mix") == 0);
}

TEST(the_mix_rides_a_queued_buffer)
{
    size_t i;

    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, 100, 8000, 0), 0);
    CHECK_EQ(audio_pcm_queue(play + 100, 700), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    tick(10);
    run_us(110000);
    CHECK_EQ(pwm_count, sizeof(play));
    for (i = 0; i < pwm_count; i++) {
        int m = i < sizeof(over) ? over[i] - 128 : 0;

        CHECK_EQ(pwm_samples[i], clamp(play[i] + m));
    }
}

TEST(clamped_at_both_ends_before_the_volume)
{
    static uint8_t hi[4] = { 250, 200, 128, 5 };
    static uint8_t mhi[4] = { 250, 200, 128, 5 };
    size_t i;

    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(hi, sizeof(hi), 8000, 0), 0);
    CHECK_EQ(audio_pcm_mix(mhi, sizeof(mhi)), 0);
    tick(10);
    run_us(2000);
    CHECK_EQ(pwm_count, 4);
    CHECK_EQ(pwm_samples[0], 255);
    CHECK_EQ(pwm_samples[1], 255);
    CHECK_EQ(pwm_samples[2], 128);
    CHECK_EQ(pwm_samples[3], 0);

    /* The volume scales the clamped sum. */
    fresh(AUDIO_BACKEND_PWM);
    audio_set_volume(50);
    CHECK_EQ(audio_play_pcm(hi, sizeof(hi), 8000, 0), 0);
    CHECK_EQ(audio_pcm_mix(mhi, sizeof(mhi)), 0);
    tick(10);
    run_us(2000);
    for (i = 0; i < 4; i++) {
        uint8_t sum = (uint8_t)clamp(hi[i] + mhi[i] - 128);

        CHECK_EQ(pwm_samples[i], audio_pcm_scale(sum, 50));
    }
    CHECK_EQ(pwm_samples[0], 192);
    CHECK_EQ(pwm_samples[3], 64);
}

TEST(a_mix_over_a_looped_play)
{
    char want[160];

    fill();
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, 80, 8000, AUDIO_LOOP), 0);
    tick(10);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    run_us(30000);
    CHECK(pcm.active);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    audio_stop();
    tick(10);
    audio_trace_flush();
    /* The looped play's line ends with loop=1; no other line has it. */
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=80 vol=100 crc=%08x t_ms=1010 loop=1\n",
             (unsigned)crc_of(play, 80));
    CHECK(strncmp(out, want, strlen(want)) == 0);
    CHECK(strstr(out + strlen(want), "loop") == 0);
}

/* A tick callback that starts a play, then a mix, on the given ticks. */
static int hook_ret_play = 1;
static int hook_ret_mix = 1;
static int hook_runs;

static void hook(void)
{
    hook_runs++;
    if (hook_runs == 1) {
        hook_ret_play = audio_play_pcm(play, sizeof(play), 8000, 0);
    } else if (hook_runs == 3) {
        hook_ret_mix = audio_pcm_mix(over, sizeof(over));
    }
}

TEST(calls_from_a_tick_callback)
{
    char want[512];

    fill();
    fresh(AUDIO_BACKEND_TRACE);
    hook_runs = 0;
    tick_hook = hook;
    /* The callback runs before the audio tick: the play starts on that tick. */
    tick(10);
    CHECK_EQ(hook_ret_play, 0);
    run_us(10000);
    tick(0);
    tick(0);
    CHECK_EQ(hook_ret_mix, 0);
    run_us(100000);
    tick(10);
    tick_hook = 0;
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_mix len=200 crc=%08x t_ms=1020\n"
             "@RC stage=host ev=audio op=pcm_end n=800 last=799 t_ms=1110\n",
             (unsigned)crc_of(play, sizeof(play)), (unsigned)crc_of(over, sizeof(over)));
    CHECK_STR(out, want);
    CHECK(!audio_busy());
}

/*
 * The compare is overdue when the overlay joins: the IRQ for sample 80
 * has not run, and the clock is `k` samples and 60 us past it. The
 * overlay still starts at the play's next grid point after the call, and
 * every one of its bytes sounds, for k under, at and over its length.
 */
TEST(a_mix_joins_after_an_overdue_compare)
{
    static const uint32_t ks[] = { 0, 1, 30, 300 };
    char want[512];
    size_t i;
    size_t j;

    fill();
    for (i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        uint32_t k = ks[i];
        uint32_t p = 80 + k;            /* the play sample the late IRQ outputs */
        uint32_t join_us = 10000 + (k + 1) * 125;

        fresh(AUDIO_BACKEND_PWM);
        CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
        tick(10);
        run_us(10000);
        CHECK_EQ(pwm_count, 80);
        fake_step(k * 125 + 60);
        CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
        run_us(200000);
        CHECK(!pcm.active);
        CHECK_EQ(pwm_samples[80], play[p]);
        CHECK_EQ(pwm_count, 81 + (sizeof(play) - p - 1));
        for (j = 0; j + 81 < pwm_count; j++) {
            uint32_t want_s = clamp(play[p + 1 + j] + (j < sizeof(over) ? over[j] - 128 : 0));

            if (pwm_samples[81 + j] != want_s) {
                RC_FAIL("k %u, overlay sample %u: %u, want %u", (unsigned)k, (unsigned)j,
                        (unsigned)pwm_samples[81 + j], (unsigned)want_s);
            }
        }

        fresh(AUDIO_BACKEND_TRACE);
        CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
        tick(10);
        run_us(10000);
        fake_step(k * 125 + 60);
        CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
        audio_trace_flush();
        snprintf(want, sizeof(want),
                 "@RC stage=host ev=audio op=pcm rate=8000 len=800 vol=100 crc=%08x t_ms=1010\n"
                 "@RC stage=host ev=audio op=pcm_mix len=200 crc=%08x t_ms=%u\n",
                 (unsigned)crc_of(play, sizeof(play)), (unsigned)crc_of(over, sizeof(over)),
                 (unsigned)(1010 + join_us / 1000));
        CHECK_STR(out, want);
    }
}

/* The play's end has passed with its IRQ still due: a queued buffer loses no byte. */
TEST(a_queue_joins_after_an_overdue_end)
{
    size_t j;

    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    run_us(99900);
    CHECK_EQ(pwm_count, sizeof(play));
    fake_step(300);
    CHECK_EQ(audio_pcm_queue(over, sizeof(over)), 0);
    run_us(100000);
    CHECK(!pcm.active);
    CHECK_EQ(pwm_count, sizeof(play) + sizeof(over));
    for (j = 0; j < sizeof(over); j++) {
        CHECK_EQ(pwm_samples[sizeof(play) + j], over[j]);
    }
}

/*
 * The overlay joins a running play at every microsecond inside one sample
 * period, at several rates, one of them odd. Over a play at the midpoint,
 * the output is the overlay alone: each byte sounds exactly once, in
 * order, from the play's next sample on. A second clock for the overlay
 * would repeat a byte and skip the next at rates like 22050 Hz.
 */
TEST(a_mix_stays_on_the_play_grid_at_every_join_offset)
{
    static const uint32_t rates[] = { 8000, 11025, 16000, 22050, 37500, 7993 };
    static uint8_t mid[3750];  /* 100 ms at the highest rate */
    static uint8_t ramp_up[64];
    size_t i, j;

    memset(mid, 128, sizeof(mid));
    for (j = 0; j < sizeof(ramp_up); j++) {
        ramp_up[j] = (uint8_t)(j + 1);
    }
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        uint32_t r = rates[i];
        uint32_t period = (1000000u + r - 1) / r;
        uint32_t d;

        for (d = 0; d < period; d++) {
            size_t at;

            fresh(AUDIO_BACKEND_PWM);
            CHECK_EQ(audio_play_pcm(mid, r / 10, r, 0), 0);
            tick(10);
            run_us(5000);
            /* Up to a grid point, its sample output, then d us into the period. */
            run_us(fake_due - fake_us);
            pcm_isr(IRQ_TIMER3, 0);
            at = pwm_count;
            fake_step(d);
            CHECK_EQ(audio_pcm_mix(ramp_up, sizeof(ramp_up)), 0);
            run_us(200000);
            CHECK(!pcm.active);
            CHECK_EQ(pwm_count, r / 10);
            for (j = 0; j < pwm_count; j++) {
                uint8_t s = j >= at && j < at + sizeof(ramp_up) ? ramp_up[j - at] : 128;

                if (pwm_samples[j] != audio_pcm_duty(s, AUDIO_PCM_PWM_RANGE)) {
                    RC_FAIL("rate %u, d %u, sample %u: %u, want %u", (unsigned)r, (unsigned)d,
                            (unsigned)j, (unsigned)pwm_samples[j],
                            (unsigned)audio_pcm_duty(s, AUDIO_PCM_PWM_RANGE));
                    break;
                }
            }
        }
    }
}

/*
 * A play and an overlay at the top rate, 37500 Hz, one sample per PWM
 * carrier period. Every play byte sounds once, in order, with the overlay
 * added from the play's next sample on.
 */
TEST(a_play_and_a_mix_at_the_top_rate)
{
    static uint8_t top[3750];   /* 100 ms at 37500 Hz */
    static uint8_t add[1500];   /* 40 ms */
    size_t i;
    size_t at;

    for (i = 0; i < sizeof(top); i++) {
        top[i] = (uint8_t)(60 + i % 137);
    }
    for (i = 0; i < sizeof(add); i++) {
        add[i] = (uint8_t)(100 + i % 57);
    }
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(top, sizeof(top), AUDIO_PCM_RATE_MAX, 0), 0);
    tick(10);
    run_us(10000);
    at = pwm_count;
    CHECK_EQ(audio_pcm_mix(add, sizeof(add)), 0);
    run_us(100000);
    CHECK(!pcm.active);
    CHECK_EQ(pwm_count, sizeof(top));
    for (i = 0; i < pwm_count; i++) {
        int m = i >= at && i < at + sizeof(add) ? add[i - at] - 128 : 0;

        if (pwm_samples[i] != clamp(top[i] + m)) {
            RC_FAIL("sample %u: %u, want %u", (unsigned)i, (unsigned)pwm_samples[i],
                    (unsigned)clamp(top[i] + m));
            break;
        }
    }
    CHECK_EQ(audio_play_pcm(top, sizeof(top), AUDIO_PCM_RATE_MAX + 1, AUDIO_PRIORITY),
             AUDIO_ERR_RANGE);
}

/*
 * The end window: the play's last sample has sounded and its end IRQ is
 * still due. The clock sits `late` us past that due time, the IRQ not run.
 */
static void to_end_window(uint32_t last, uint32_t late)
{
    unsigned guard = 0;

    while (pcm.active && pcm.last != last && guard++ < 10000) {
        fake_step(fake_due - fake_us);
        pcm_isr(IRQ_TIMER3, 0);
    }
    CHECK(pcm.active);
    CHECK_EQ(pcm.last, last);
    fake_step(fake_due - fake_us + late);
}

TEST(a_mix_after_a_one_shot_last_sample_is_refused)
{
    static const uint32_t lates[] = { 0, 1, 124, 125, 100000 };
    size_t i;

    fill();
    for (i = 0; i < sizeof(lates) / sizeof(lates[0]); i++) {
        fresh(AUDIO_BACKEND_TRACE);
        CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
        tick(10);
        to_end_window(sizeof(play) - 1, lates[i]);
        clear_logs();
        CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
        CHECK_EQ(audio_pcm_mix_busy(), 0);
        CHECK_EQ(audio_busy(), 1);
        CHECK_EQ(mix.active, 0);
        CHECK_EQ(mix_req_set, 0);
        audio_trace_flush();
        CHECK(strstr(out, "op=pcm_mix") == 0);
        pcm_isr(IRQ_TIMER3, 0);
        CHECK(!pcm.active);
        CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
    }
}

TEST(an_overlay_in_the_end_window_keeps_busy_and_refuses_a_second)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    run_us(99000);
    CHECK_EQ(audio_pcm_mix(longer, sizeof(longer)), 0);
    to_end_window(sizeof(play) - 1, 0);
    /* No play sample left: the state error comes before the busy one. */
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_STATE);
    CHECK_EQ(audio_pcm_mix_busy(), 1);
    pcm_isr(IRQ_TIMER3, 0);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
}

TEST(a_mix_after_a_looped_last_sample_sounds_from_byte_0)
{
    char want[160];

    fill();
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, AUDIO_LOOP), 0);
    tick(10);
    to_end_window(sizeof(play) - 1, 0);
    clear_logs();
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    audio_trace_flush();
    snprintf(want, sizeof(want), "@RC stage=host ev=audio op=pcm_mix len=%u crc=%08x t_ms=1110\n",
             (unsigned)sizeof(over), (unsigned)crc_of(over, sizeof(over)));
    CHECK_STR(out, want);
    backend = AUDIO_BACKEND_PWM;
    pwm_count = 0;
    pcm_isr(IRQ_TIMER3, 0);
    CHECK_EQ(pcm.last, 0);
    CHECK_EQ(pwm_count, 1);
    CHECK_EQ(pwm_samples[0], audio_pcm_duty((uint8_t)clamp(play[0] + over[0] - 128),
                                            AUDIO_PCM_PWM_RANGE));
}

TEST(a_mix_after_the_last_sample_before_a_queued_buffer_sounds_in_it)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    CHECK_EQ(audio_pcm_queue(longer, sizeof(longer)), 0);
    tick(10);
    to_end_window(sizeof(play) - 1, 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 1);
    pwm_count = 0;
    pcm_isr(IRQ_TIMER3, 0);
    CHECK_EQ(pcm.last, 0);
    CHECK_EQ(pcm.data, longer);
    CHECK_EQ(pwm_count, 1);
    CHECK_EQ(pwm_samples[0], audio_pcm_duty((uint8_t)clamp(longer[0] + over[0] - 128),
                                            AUDIO_PCM_PWM_RANGE));
}

TEST(a_queue_after_a_one_shot_last_sample_sounds_its_byte_0)
{
    fill();
    fresh(AUDIO_BACKEND_PWM);
    CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
    tick(10);
    to_end_window(sizeof(play) - 1, 300);
    CHECK_EQ(audio_pcm_can_queue(), 1);
    CHECK_EQ(audio_pcm_queue(longer, sizeof(longer)), 0);
    /* Byte 0 sounds at the next grid sample. */
    pwm_count = 0;
    pcm_isr(IRQ_TIMER3, 0);
    CHECK_EQ(pwm_count, 0);
    run_us(125);
    CHECK_EQ(pwm_count, 1);
    CHECK(pcm.active);
    CHECK_EQ(pcm.data, longer);
    CHECK_EQ(pcm.last, 0);
    CHECK_EQ(pwm_samples[0], audio_pcm_duty(longer[0], AUDIO_PCM_PWM_RANGE));
}
