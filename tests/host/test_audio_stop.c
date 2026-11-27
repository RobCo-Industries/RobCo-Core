/*
 * The trace rule for stops in src/drv/audio.c: a play that was started
 * and ends by a stop call or a replacement prints one op=stop, whether a
 * tick runs between the calls or not. The test builds the driver and the
 * trace backend in, stubs the timer, IRQ and PWM, and runs the tick by hand.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include <stdio.h>
#include <string.h>

#include <robco/board.h>

#include "rc_test.h"

static uint32_t fake_us;
static uint32_t ticks;

int irq_ready(void) { return 1; }
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t state) { (void)state; }
int irq_register(unsigned irq, irq_handler_t fn, void *arg)
{
    (void)irq;
    (void)fn;
    (void)arg;
    return 0;
}
void irq_enable(unsigned irq) { (void)irq; }
void irq_disable(unsigned irq) { (void)irq; }
uint32_t tick_count(void) { return ++ticks; }
uint32_t tick_ms(void) { return 0; }
int tick_register(void (*fn)(void))
{
    (void)fn;
    return 0;
}
uint32_t timer_now_lo(void) { return fake_us; }
uint64_t timer_now_us(void) { return fake_us; }
void timer_compare_clear(unsigned ch) { (void)ch; }
int timer_compare_arm(unsigned ch, uint32_t clo)
{
    (void)ch;
    (void)clo;
    return 0;
}
uint32_t timer_oneshot_arm(unsigned ch, uint32_t clo)
{
    (void)ch;
    return clo;
}

const robco_board_t *board_get(void)
{
    static robco_board_t b;

    return &b;
}

/* PWM calls, as text: what a PWM run would write, in order. */
static char pwm_log[4096];

static void pwm_note(const char *s)
{
    if (strlen(pwm_log) + strlen(s) + 1 < sizeof(pwm_log)) {
        strcat(pwm_log, s);
    }
}

int audio_pwm_probe(void) { return AUDIO_PROBE_OK; }
int audio_pwm_setup(int p0, unsigned f0, int p1, unsigned f1)
{
    (void)p0;
    (void)f0;
    (void)p1;
    (void)f1;
    return 0;
}
void audio_pwm_out(uint32_t hz, unsigned vol)
{
    char b[48];

    snprintf(b, sizeof(b), "out %u %u;", (unsigned)hz, vol);
    pwm_note(b);
}
void audio_pwm_pcm_mode(void) { pwm_note("pcm_mode;"); }
void audio_pwm_pcm_mode_at(uint32_t dat)
{
    (void)dat;
    pwm_note("pcm_mode_at;");
}
void audio_pwm_sample(uint32_t dat)
{
    (void)dat;
    pwm_note("sample;");
}

static char out[4096];

static void capture(char c)
{
    size_t n = strlen(out);

    if (n + 1 < sizeof(out)) {
        out[n] = c;
        out[n + 1] = '\0';
    }
}

/* One tick run, `ms` after the last one. */
static void tick(uint32_t ms)
{
    fake_us += ms * 1000u;
    audio_tick();
}

static int started;

/* A fresh driver on `which` backend, at clock 1000 ms. */
static void fresh(int which)
{
    if (!started) {
        k_console_set(capture);
        CHECK_EQ(audio_select(AUDIO_BACKEND_TRACE), 0);
        CHECK_EQ(audio_init(), 0);
        started = 1;
    }
    backend = which;
    k_memset((void *)&req, 0, sizeof(req));
    k_memset(&seq, 0, sizeof(seq));
    k_memset(&pcm, 0, sizeof(pcm));
    pcm_cut = 0;
    sounding = 0;
    playing = 0;
    clock_ms = 1000;
    acc_us = 0;
    last_us = fake_us;
    audio_set_volume(100);
    audio_trace_flush();
    out[0] = '\0';
    pwm_log[0] = '\0';
}

static uint8_t buf_a[8000];
static uint8_t buf_b[800];
static const audio_note_t seq_rest[] = { { 440, 100 }, { 0, 100 } };
static const audio_note_t seq_new[] = { { 660, 300 }, { 770, 300 } };

enum { P_NOTHING, P_TONE, P_SEQ, P_REST, P_PCM, P_QUEUED, P_COUNT };
enum { A_STOP, A_TONE, A_SEQ, A_PCM, A_COUNT };
enum { T_ALONE, T_SAME_TICK, T_SPLIT, T_COUNT };

static const char *const p_name[] = { "nothing", "tone", "seq", "rest", "pcm", "queued" };
static const char *const a_name[] = { "stop", "tone", "seq", "pcm" };
static const char *const t_name[] = { "alone", "same tick", "split" };

/* Starts `p` and runs ticks until it plays as named. */
static void start(int p)
{
    switch (p) {
    case P_TONE:
        CHECK_EQ(audio_tone(440, 10000), 0);
        break;
    case P_SEQ:
    case P_REST:
        CHECK_EQ(audio_play(seq_rest, 2, AUDIO_LOOP), 0);
        break;
    case P_PCM:
        CHECK_EQ(audio_play_pcm(buf_a, sizeof(buf_a), 8000, 0), 0);
        break;
    case P_QUEUED:
        CHECK_EQ(audio_play_pcm(buf_a, sizeof(buf_a), 8000, 0), 0);
        CHECK_EQ(audio_pcm_queue(buf_b, sizeof(buf_b)), 0);
        break;
    default:
        break;
    }
    tick(10);
    if (p == P_REST) {
        tick(110);
        CHECK(seq.active && seq.hz == 0);
    }
    if (p == P_SEQ) {
        CHECK(seq.active && seq.hz == 440);
    }
}

static void act(int a)
{
    switch (a) {
    case A_STOP:
        audio_stop();
        break;
    case A_TONE:
        CHECK_EQ(audio_tone(880, 5000), 0);
        break;
    case A_SEQ:
        CHECK_EQ(audio_play(seq_new, 2, AUDIO_PRIORITY), 0);
        break;
    default:
        CHECK_EQ(audio_play_pcm(buf_b, sizeof(buf_b), 8000, AUDIO_PRIORITY), 0);
        break;
    }
}

/*
 * Runs p, then a in timing t, with no time passing after the start. Puts
 * the trace lines printed after the start in `lines`.
 */
static void run(int which, int p, int a, int t, char *lines, size_t size)
{
    fresh(which);
    start(p);
    audio_trace_flush();
    out[0] = '\0';
    pwm_log[0] = '\0';
    if (t == T_ALONE) {
        act(a);
    } else {
        audio_stop();
        if (t == T_SPLIT) {
            tick(0);
        }
        act(a);
    }
    tick(0);
    audio_trace_flush();
    /* A copy, cut to size. snprintf with "%s" here trips GCC's
     * -Wformat-truncation under the CI sanitizer flags. */
    const char *log = which == AUDIO_BACKEND_PWM ? pwm_log : out;
    size_t n = strlen(log);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(lines, log, n);
    lines[n] = '\0';
}

/* What the rule says: one stop if something played, then the new start. */
static void expected(int p, int a, uint32_t t_ms, char *lines, size_t size)
{
    char stop[96] = "";
    char go[128] = "";

    if (p != P_NOTHING) {
        snprintf(stop, sizeof(stop), "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=%u\n",
                 (unsigned)t_ms);
    }
    switch (a) {
    case A_TONE:
        snprintf(go, sizeof(go), "@RC stage=host ev=audio op=tone hz=880 vol=100 t_ms=%u\n",
                 (unsigned)t_ms);
        break;
    case A_SEQ:
        snprintf(go, sizeof(go), "@RC stage=host ev=audio op=tone hz=660 vol=100 t_ms=%u\n",
                 (unsigned)t_ms);
        break;
    case A_PCM:
        snprintf(go, sizeof(go),
                 "@RC stage=host ev=audio op=pcm rate=8000 len=%u vol=100 crc=%08x t_ms=%u\n",
                 (unsigned)sizeof(buf_b), (unsigned)crc32(buf_b, sizeof(buf_b)),
                 (unsigned)t_ms);
        break;
    default:
        break;
    }
    snprintf(lines, size, "%s%s", stop, go);
}

static void fill(void)
{
    size_t i;

    for (i = 0; i < sizeof(buf_a); i++) {
        buf_a[i] = (uint8_t)(i * 7u);
    }
    for (i = 0; i < sizeof(buf_b); i++) {
        buf_b[i] = (uint8_t)(i * 13u + 1u);
    }
}

TEST(every_stop_traced_once_in_every_timing)
{
    char got[1024], want[1024];
    int p, a, t;

    fill();
    for (p = 0; p < P_COUNT; p++) {
        for (a = 0; a < A_COUNT; a++) {
            expected(p, a, p == P_REST ? 1120 : 1010, want, sizeof(want));
            for (t = 0; t < T_COUNT; t++) {
                run(AUDIO_BACKEND_TRACE, p, a, t, got, sizeof(got));
                if (strcmp(got, want) != 0) {
                    RC_FAIL("%s, %s, %s:\ngot:\n%swant:\n%s", p_name[p], a_name[a], t_name[t],
                            got, want);
                }
            }
        }
    }
}

TEST(nothing_playing_after_a_natural_end_prints_no_stop)
{
    fill();
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_tone(440, 30), 0);
    tick(10);
    tick(40);
    audio_trace_flush();
    CHECK_STR(out, "@RC stage=host ev=audio op=tone hz=440 vol=100 t_ms=1010\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1040\n");
    out[0] = '\0';
    audio_stop();
    tick(0);
    audio_trace_flush();
    CHECK_STR(out, "");
}

TEST(a_rest_and_a_stop_in_it_each_print_a_line)
{
    fill();
    fresh(AUDIO_BACKEND_TRACE);
    CHECK_EQ(audio_play(seq_rest, 2, 0), 0);
    tick(10);
    tick(110);
    audio_stop();
    tick(10);
    audio_trace_flush();
    CHECK_STR(out, "@RC stage=host ev=audio op=tone hz=440 vol=100 t_ms=1010\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1110\n"
                   "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1130\n");
}

/*
 * The PWM writes for every case. A build of this test against the driver
 * before the rule printed the same table: the rule only adds trace records.
 */
static const char *const pwm_want[P_COUNT][A_COUNT] = {
    /* nothing */ { "", "out 880 100;", "out 660 100;", "pcm_mode;sample;" },
    /* tone */    { "out 0 100;", "out 880 100;", "out 660 100;", "pcm_mode;sample;" },
    /* seq */     { "out 0 100;", "out 880 100;", "out 660 100;", "pcm_mode;sample;" },
    /* rest */    { "", "out 880 100;", "out 660 100;", "pcm_mode;sample;" },
    /* pcm: the call cuts, then the tick halts */
    /* pcm */     { "out 0 0;out 0 0;", "out 0 0;out 0 0;out 880 100;",
                    "out 0 0;out 0 0;out 660 100;", "out 0 0;out 0 0;pcm_mode;sample;" },
    /* queued */  { "out 0 0;out 0 0;", "out 0 0;out 0 0;out 880 100;",
                    "out 0 0;out 0 0;out 660 100;", "out 0 0;out 0 0;pcm_mode;sample;" },
};

TEST(pwm_writes_unchanged_by_the_rule)
{
    char got[1024];
    int p, a;

    fill();
    for (p = 0; p < P_COUNT; p++) {
        for (a = 0; a < A_COUNT; a++) {
            run(AUDIO_BACKEND_PWM, p, a, T_ALONE, got, sizeof(got));
            if (strcmp(got, pwm_want[p][a]) != 0) {
                RC_FAIL("%s, %s: got \"%s\", want \"%s\"", p_name[p], a_name[a], got,
                        pwm_want[p][a]);
            }
        }
    }
}
