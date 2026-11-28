/*
 * audio_play_voice() and the voice engine in src/lib/audio_voice.c: the
 * sine table, the per-sample integers, the skip rule, the check errors,
 * one rounding with an overlay and the end of the play. The test builds
 * the driver and the trace backend in, as test_audio_mix.c does.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

/* A rate with a whole number of us per sample keeps the fake clock simple. */
#define RATE        8000u

/* floor(a / b) for b > 0, whatever the sign of a. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* Sample i of a span, written apart from the library: 64-bit sums, explicit floors. */
static int32_t ref_at(const audio_voice_span_t *s, uint32_t i)
{
    uint64_t tri = (uint64_t)i * (i ? i - 1 : 0) / 2;
    uint32_t ph = (uint32_t)((uint64_t)s->phase + (uint64_t)i * s->inc +
                             (uint64_t)(uint32_t)s->dinc * tri);
    int64_t a = floor_div((int64_t)s->amp + (int64_t)i * s->damp, 256);
    int64_t q = floor_div((int64_t)audio_sine[ph >> 22] * a, 32768);

    if (s->p2) {
        uint32_t ph2 = (uint32_t)(ph * 2u);
        int64_t h = floor_div((int64_t)audio_sine[ph2 >> 22] * a, 32768);

        q += floor_div(h * (int64_t)s->p2, 32768);
    }
    return (int32_t)q;
}

/* The phase and step at the end of a span: what the next span starts with. */
static void carry(const audio_voice_span_t *s, audio_voice_span_t *next)
{
    uint64_t n = s->n;

    next->phase = (uint32_t)((uint64_t)s->phase + n * s->inc +
                             (uint64_t)(uint32_t)s->dinc * (n * (n - 1) / 2));
    next->inc = (uint32_t)((int64_t)s->inc + (int64_t)n * s->dinc);
}

TEST(sine_table_crc)
{
    CHECK_EQ(crc32(audio_sine, sizeof(audio_sine)), 0x05067686u);
    CHECK_EQ(sizeof(audio_sine), 2048u);
    CHECK_EQ(audio_sine[0], 0);
    CHECK_EQ(audio_sine[256], 32767);
    CHECK_EQ(audio_sine[768], -32767);
}

TEST(test_vector)
{
    static const audio_voice_span_t s = { 1000, 0, 0x10000000u, 0, 20 << 16, 0, 0 };
    audio_voice_t v;

    CHECK_EQ(audio_voice_span_at(&s, 4), 5119);
    CHECK_EQ(audio_voice_span_at(&s, 5), 4730);
    audio_voice_start(&v, &s, 1);
    CHECK_EQ(audio_voice_at(&v, 4), 5119);
    CHECK_EQ(audio_voice_at(&v, 5), 4730);
}

TEST(second_partial)
{
    /* 45 degrees: the fundamental at 23170, the partial at its peak. */
    static const audio_voice_span_t s = { 10, 0x20000000u, 0, 0, 20 << 16, 0, 16384 };
    static const audio_voice_span_t g = { 4000, 123456789u, 9000000u, 700, 20 << 16, -300, 6000 };
    uint32_t i;

    /* (23170 * 5120) >> 15 = 3620, ((32767 * 5120) >> 15) * 16384 >> 15 = 2559. */
    CHECK_EQ(audio_voice_span_at(&s, 0), 3620 + 2559);
    CHECK_EQ(audio_voice_span_at(&s, 9), 3620 + 2559);
    for (i = 0; i < g.n; i++) {
        CHECK_EQ(audio_voice_span_at(&g, i), ref_at(&g, i));
    }
}

TEST(level_ramp_to_zero)
{
    /* At the sine's peak all along, from 100 steps to 0 in 100 samples. */
    static const audio_voice_span_t s = { 101, 0x40000000u, 0, 0, 100 << 16, -65536, 0 };
    int32_t last = 0x7fffffff;
    uint32_t i;

    CHECK_EQ(audio_voice_check(&s, 1, RATE, 0, 0), 0);
    for (i = 0; i < s.n; i++) {
        int32_t q = audio_voice_span_at(&s, i);

        CHECK(q <= last);
        CHECK_EQ(q, ref_at(&s, i));
        last = q;
    }
    CHECK_EQ(audio_voice_span_at(&s, 0), (32767 * 25600) >> 15);
    CHECK_EQ(last, 0);
}

TEST(phase_carries_across_spans)
{
    /* One gliding span, then the same cut in three with the phase carried. */
    static const audio_voice_span_t whole = { 3000, 7u, 50000000u, 9000, 30 << 16, -100, 4000 };
    audio_voice_span_t parts[3];
    audio_voice_t v;
    uint32_t i;

    parts[0] = whole;
    parts[0].n = 1000;
    parts[1] = parts[0];
    parts[1].n = 700;
    carry(&parts[0], &parts[1]);
    parts[1].amp = whole.amp + 1000 * whole.damp;
    parts[2] = parts[1];
    parts[2].n = 1300;
    carry(&parts[1], &parts[2]);
    parts[2].amp = whole.amp + 1700 * whole.damp;
    CHECK_EQ(audio_voice_check(parts, 3, RATE, 0, 0), 0);
    audio_voice_start(&v, parts, 3);
    for (i = 0; i < whole.n; i++) {
        CHECK_EQ(audio_voice_at(&v, i), audio_voice_span_at(&whole, i));
    }
    CHECK_EQ(audio_voice_at(&v, whole.n), 0);
}

/* Five spans, 23000 samples, every field moving. */
static const audio_voice_span_t walk[] = {
    { 5000, 0, 100000000u, 2000, 10 << 16, 100, 8000 },
    { 1, 0x12345678u, 300000000u, -5, 40 << 16, 0, 0 },
    { 9999, 0x80000000u, 2000000000u, -1000, 60 << 16, -300, 32768 },
    { 2, 5, 1, 0, 127 << 16, -(127 << 16), 1 },
    { 7998, 0xfffffff0u, 0x7fffffffu, -3, 0, 1000, 20000 },    /* the top step, falling: a rise would leave the range */
};
#define WALK_SPANS  (sizeof(walk) / sizeof(walk[0]))
#define WALK_LEN    23000u
#define WALK_PAST   (WALK_LEN + 20000u)

static int32_t single[WALK_PAST];

TEST(a_skip_equals_single_steps)
{
    static const uint32_t skips[] = { 1, 2, 100, 10000 };
    /* Starts: the first sample, inside a span, on and around the span edges. */
    static const uint32_t starts[] = { 0, 3, 4999, 5000, 5001, 15000, 15001, 15002, 22999 };
    audio_voice_t v;
    uint32_t len = 0;
    uint32_t i;
    size_t k, j;

    CHECK_EQ(audio_voice_check(walk, WALK_SPANS, RATE, 0, &len), 0);
    CHECK_EQ(len, WALK_LEN);
    audio_voice_start(&v, walk, WALK_SPANS);
    for (i = 0; i < WALK_PAST; i++) {
        single[i] = audio_voice_at(&v, i);
    }
    /* Single steps agree with the closed form of each span. */
    for (i = 0, k = 0, j = 0; i < WALK_LEN; i++, j++) {
        if (j == walk[k].n) {
            k++;
            j = 0;
        }
        CHECK_EQ(single[i], ref_at(&walk[k], (uint32_t)j));
    }
    for (i = WALK_LEN; i < WALK_PAST; i++) {
        CHECK_EQ(single[i], 0);
    }
    for (k = 0; k < sizeof(skips) / sizeof(skips[0]); k++) {
        for (j = 0; j < sizeof(starts) / sizeof(starts[0]); j++) {
            uint32_t pos;

            audio_voice_start(&v, walk, WALK_SPANS);
            for (pos = starts[j]; pos < WALK_PAST; pos += skips[k]) {
                int32_t q = audio_voice_at(&v, pos);

                if (q != single[pos]) {
                    RC_FAIL("skip %u from %u: sample %u is %d, want %d", (unsigned)skips[k],
                            (unsigned)starts[j], (unsigned)pos, (int)q, (int)single[pos]);
                    return;
                }
                /* The bytes too, at a volume and with an overlay byte. */
                CHECK_EQ(audio_voice_out(q, 131, 77), audio_voice_out(single[pos], 131, 77));
            }
        }
    }
}

/* Every bad argument the check lists, each alone. */
static audio_voice_span_t many[32769];

TEST(check_errors)
{
    audio_voice_span_t s = { 100, 0, 1000, 0, 10 << 16, 0, 0 };
    audio_voice_span_t b;
    uint32_t len = 0;
    size_t i;

    CHECK_EQ(audio_voice_check(&s, 1, RATE, 0, &len), 0);
    CHECK_EQ(len, 100u);
    CHECK_EQ(audio_voice_check(&s, 1, RATE, AUDIO_PRIORITY, 0), 0);
    CHECK_EQ(audio_voice_check(rc_launder(NULL), 1, RATE, 0, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_voice_check(&s, 0, RATE, 0, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_voice_check(&s, 1, RATE, AUDIO_LOOP, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_voice_check(&s, 1, RATE, 4, 0), AUDIO_ERR_ARG);
    b = s; b.n = 0;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.n = AUDIO_VOICE_N_MAX + 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.n = AUDIO_VOICE_N_MAX;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b = s; b.inc = 0x80000000u;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.inc = AUDIO_VOICE_INC_MAX;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    /* The phase step leaves its range at the span's last sample, below and above. */
    b = s; b.n = 2; b.inc = 0; b.dinc = -1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.n = 2; b.inc = AUDIO_VOICE_INC_MAX; b.dinc = 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.n = 1; b.inc = AUDIO_VOICE_INC_MAX; b.dinc = 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b = s; b.n = 2; b.inc = 1; b.dinc = -1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    /* A full span: the last sample's step lands on each limit, then one past it. */
    b = s; b.n = AUDIO_VOICE_N_MAX; b.inc = AUDIO_VOICE_N_MAX - 1; b.dinc = -1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b.inc--;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.n = AUDIO_VOICE_N_MAX; b.inc = AUDIO_VOICE_INC_MAX - (AUDIO_VOICE_N_MAX - 1); b.dinc = 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b.inc++;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    /* The largest moves do not wrap the check. */
    b = s; b.n = AUDIO_VOICE_N_MAX; b.inc = AUDIO_VOICE_INC_MAX; b.dinc = INT32_MIN;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.n = AUDIO_VOICE_N_MAX; b.inc = 0; b.dinc = INT32_MAX;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.amp = -1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.amp = AUDIO_VOICE_AMP_MAX + 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.amp = AUDIO_VOICE_AMP_MAX;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    /* The level leaves its range at the span's last sample, below and above. */
    b = s; b.amp = 99; b.damp = -1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b.amp = 98;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.amp = AUDIO_VOICE_AMP_MAX - 99; b.damp = 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b.amp--;
    b.damp = 2;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    /* One sample: damp never applies. */
    b = s; b.n = 1; b.damp = INT32_MIN;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    b = s; b.p2 = AUDIO_VOICE_P2_MAX + 1;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), AUDIO_ERR_ARG);
    b = s; b.p2 = AUDIO_VOICE_P2_MAX;
    CHECK_EQ(audio_voice_check(&b, 1, RATE, 0, 0), 0);
    /* A bad span after good ones. */
    many[0] = s;
    many[1] = s;
    many[1].n = 0;
    CHECK_EQ(audio_voice_check(many, 2, RATE, 0, 0), AUDIO_ERR_ARG);
    /* The sum of n: 32767 spans of 65535 fit, one more passes AUDIO_PCM_LEN_MAX. */
    for (i = 0; i < 32769; i++) {
        many[i] = s;
        many[i].n = AUDIO_VOICE_N_MAX;
    }
    many[32768].n = 32767;
    CHECK_EQ(audio_voice_check(many, 32769, RATE, 0, &len), 0);
    CHECK_EQ(len, AUDIO_PCM_LEN_MAX);
    many[32768].n = 32768;
    CHECK_EQ(audio_voice_check(many, 32769, RATE, 0, 0), AUDIO_ERR_ARG);
    /* The rate, after every span passes. */
    CHECK_EQ(audio_voice_check(&s, 1, AUDIO_PCM_RATE_MIN - 1, 0, 0), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_voice_check(&s, 1, AUDIO_PCM_RATE_MAX + 1, 0, 0), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_voice_check(&s, 1, AUDIO_PCM_RATE_MAX, 0, 0), 0);
    CHECK_EQ(audio_voice_check(&b, 1, 1, AUDIO_LOOP, 0), AUDIO_ERR_ARG);
}

TEST(driver_errors)
{
    static const audio_voice_span_t s = { 100, 0, 1000, 0, 10 << 16, 0, 0 };
    static const audio_voice_span_t bad = { 0, 0, 1000, 0, 10 << 16, 0, 0 };

    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_voice(rc_launder(NULL), 1, RATE, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_play_voice(&s, 0, RATE, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_play_voice(&s, 1, RATE, AUDIO_LOOP), AUDIO_ERR_ARG);
    CHECK_EQ(audio_play_voice(&bad, 1, RATE, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_play_voice(&s, 1, 100, 0), AUDIO_ERR_RANGE);
    pcm_irq = 0;
    CHECK_EQ(audio_play_voice(&s, 1, RATE, 0), AUDIO_ERR_NO_IRQ);
    pcm_irq = 1;
    CHECK_EQ(audio_busy(), 0);
    CHECK_EQ(audio_play_voice(&s, 1, RATE, 0), 0);
    CHECK_EQ(audio_play_voice(&s, 1, RATE, 0), AUDIO_ERR_BUSY);
    CHECK_EQ(audio_play_voice(&s, 1, RATE, AUDIO_PRIORITY), 0);
    backend = -1;
    CHECK_EQ(audio_play_voice(&s, 1, RATE, 0), AUDIO_ERR_NO_INIT);
}

/* The driver refuses a span whose pitch leaves its range, at either end, and plays nothing. */
TEST(the_driver_refuses_a_pitch_that_leaves_its_range)
{
    static const audio_voice_span_t low = { 2, 0, 0, -1, 10 << 16, 0, 0 };
    static const audio_voice_span_t high = { 2, 0, AUDIO_VOICE_INC_MAX, 1, 10 << 16, 0, 0 };

    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_voice(&low, 1, RATE, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_play_voice(&high, 1, RATE, 0), AUDIO_ERR_ARG);
    CHECK_EQ(audio_busy(), 0);
    tick(10);
    audio_trace_flush();
    CHECK_STR(out, "");
}

TEST(one_rounding)
{
    /* 0.5 step plus 2 steps at 50 %: 1.25 once, 1.5 if the voice rounded first. */
    CHECK_EQ(audio_voice_out(128, 130, 50), 129);
    CHECK_EQ(audio_pcm_scale((uint8_t)(128 + 1 + 2), 50), 130);
    /* No overlay, full volume: the voice alone, rounded once. */
    CHECK_EQ(audio_voice_out(128, 128, 100), 129);
    CHECK_EQ(audio_voice_out(127, 128, 100), 128);
    CHECK_EQ(audio_voice_out(-128, 128, 100), 127);
    CHECK_EQ(audio_voice_out(-127, 128, 100), 128);
    /* Clamped at both ends, after the volume. */
    CHECK_EQ(audio_voice_out(127 << 8, 255, 100), 255);
    CHECK_EQ(audio_voice_out(-(127 << 8), 0, 100), 0);
    CHECK_EQ(audio_voice_out(127 << 8, 255, 40), 128 + 102);
    CHECK_EQ(audio_voice_out(0, 200, 0), 128);
    CHECK_EQ(audio_voice_out(0, 200, 150), 200);
}

TEST(one_rounding_with_an_overlay_in_the_driver)
{
    /* The sine's peak at a level of 129/256: 128 on the fine scale, half a step. */
    static const audio_voice_span_t s = { 80, 0x40000000u, 0, 0, 129 << 8, 0, 0 };
    static uint8_t over[40];
    size_t i;

    CHECK_EQ(audio_voice_span_at(&s, 0), 128);
    k_memset(over, 130, sizeof(over));
    fresh(AUDIO_BACKEND_PWM);
    audio_set_volume(50);
    CHECK_EQ(audio_play_voice(&s, 1, RATE, 0), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    tick(10);
    run_us(80 * 125 + 125);
    CHECK_EQ(pwm_count, 80u);
    for (i = 0; i < 80; i++) {
        uint32_t want = audio_pcm_duty(i < 40 ? 129 : 128, AUDIO_PCM_PWM_RANGE);

        if (pwm_samples[i] != want) {
            RC_FAIL("sample %u: duty %u, want %u", (unsigned)i, (unsigned)pwm_samples[i],
                    (unsigned)want);
            return;
        }
    }
    CHECK(strstr(pwm_log, "mid;") != 0);
}

TEST(late_irqs_in_the_driver_skip_to_the_clock)
{
    static const uint32_t gaps[] = { 125, 250, 125, 1250, 12500, 125, 375, 625000, 125 };
    uint32_t want_n = 0;
    audio_voice_t ref;
    size_t g;

    audio_voice_start(&ref, walk, WALK_SPANS);
    fresh(AUDIO_BACKEND_PWM);
    audio_set_volume(80);
    CHECK_EQ(audio_play_voice(walk, WALK_SPANS, RATE, 0), 0);
    tick(10);
    for (g = 0; g < sizeof(gaps) / sizeof(gaps[0]) && pcm.active; g++) {
        size_t before = pwm_count;

        fake_step(gaps[g]);
        pcm_isr(IRQ_TIMER3, 0);
        if (!pcm.active) {
            break;
        }
        CHECK_EQ(pwm_count, before + 1);
        /* The sample the clock asks for, from the closed form, not stepped there. */
        CHECK_EQ(pwm_samples[pwm_count - 1],
                 audio_pcm_duty(audio_voice_out(audio_voice_at(&ref, pcm.last), 128, 80),
                                AUDIO_PCM_PWM_RANGE));
        want_n++;
    }
    CHECK(want_n >= 6);
}

TEST(trace_start_mix_and_end)
{
    static const audio_voice_span_t s[2] = {
        { 500, 0, 0x10000000u, 0, 20 << 16, 0, 0 },
        { 300, 0, 0x10000000u, 0, 20 << 16, -4000, 0 },
    };
    static uint8_t over[100];
    char want[512];

    k_memset(over, 140, sizeof(over));
    fresh(AUDIO_BACKEND_TRACE);
    audio_set_volume(70);
    CHECK_EQ(audio_play_voice(s, 2, RATE, 0), 0);
    CHECK_EQ(audio_busy(), 1);
    CHECK_EQ(audio_pcm_queue(over, sizeof(over)), AUDIO_ERR_STATE);
    CHECK_EQ(audio_pcm_can_queue(), 0);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 1);
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), AUDIO_ERR_BUSY);
    tick(10);
    CHECK_EQ(audio_pcm_queue(over, sizeof(over)), AUDIO_ERR_STATE);
    CHECK_EQ(audio_pcm_can_queue(), 0);
    /* 800 samples at 8000 Hz: 100 ms, and the end IRQ one period after the last. */
    run_us(100000 + 1000);
    tick(10);
    CHECK_EQ(audio_busy(), 0);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=voice rate=8000 spans=2 len=800 vol=70 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_mix len=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_end n=800 last=799 t_ms=1110\n",
             (unsigned)crc32(s, sizeof(s)), (unsigned)crc32(over, sizeof(over)));
    CHECK_STR(out, want);
}

TEST(a_cut_prints_stop)
{
    static const audio_voice_span_t s = { 8000, 0, 0x10000000u, 0, 20 << 16, 0, 0 };

    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play_voice(&s, 1, RATE, 0), 0);
    tick(10);
    run_us(20000);
    audio_stop();
    CHECK_EQ(pcm.active, 0);
    tick(10);
    audio_trace_flush();
    CHECK(strstr(out, "op=voice rate=8000 spans=1 len=8000") != 0);
    CHECK(strstr(out, "op=stop hz=0") != 0);
    CHECK(strstr(out, "op=pcm_end") == 0);
}
