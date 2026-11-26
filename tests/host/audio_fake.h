/*
 * Stubs for host tests that build src/drv/audio.c and audio_trace.c in
 * with #include, as test_audio_stop.c does. Include this after them. The
 * timer, IRQ and tick are fake: tick() runs the tick by hand, run_us()
 * moves the clock and runs the channel 3 IRQ at each due sample. The PWM
 * backend records what it would write.
 */
#ifndef AUDIO_FAKE_H
#define AUDIO_FAKE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <robco/board.h>

#include "rc_test.h"

static uint32_t fake_us;
static uint32_t fake_hi;        /* high word of the 64-bit counter */
static uint32_t fake_ticks;
static int fake_irq3;
static uint32_t fake_due;

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
void irq_enable(unsigned irq)
{
    if (irq == IRQ_TIMER3) {
        fake_irq3 = 1;
    }
}
void irq_disable(unsigned irq)
{
    if (irq == IRQ_TIMER3) {
        fake_irq3 = 0;
    }
}
uint32_t tick_count(void) { return ++fake_ticks; }
uint32_t tick_ms(void) { return 0; }
int tick_register(void (*fn)(void))
{
    (void)fn;
    return 0;
}
uint32_t timer_now_lo(void) { return fake_us; }
uint64_t timer_now_us(void) { return ((uint64_t)fake_hi << 32) | fake_us; }
void timer_compare_clear(unsigned ch) { (void)ch; }
int timer_compare_arm(unsigned ch, uint32_t clo)
{
    (void)ch;
    fake_due = clo;
    return 0;
}
uint32_t timer_oneshot_arm(unsigned ch, uint32_t clo)
{
    (void)ch;
    fake_due = clo;
    return clo;
}

const robco_board_t *board_get(void)
{
    static robco_board_t b;

    return &b;
}

/* PWM writes other than samples, as text, and the sample duties in order. */
static char pwm_log[4096];
static uint32_t pwm_samples[4096];
static size_t pwm_count;

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
void audio_pwm_pcm_mode(void) { pwm_note("mid;"); }
void audio_pwm_pcm_mode_at(uint32_t dat)
{
    char b[32];

    snprintf(b, sizeof(b), "at %u;", (unsigned)dat);
    pwm_note(b);
}
void audio_pwm_sample(uint32_t dat)
{
    if (pwm_count < sizeof(pwm_samples) / sizeof(pwm_samples[0])) {
        pwm_samples[pwm_count] = dat;
    }
    pwm_count++;
}

static char out[8192];

static void capture(char c)
{
    size_t n = strlen(out);

    if (n + 1 < sizeof(out)) {
        out[n] = c;
        out[n + 1] = '\0';
    }
}

/* Moves the clock without any IRQ, carrying into the high word. */
static void fake_step(uint32_t us)
{
    uint32_t was = fake_us;

    fake_us += us;
    if (fake_us < was) {
        fake_hi++;
    }
}

/* Runs before audio_tick() in tick(), as another tick callback would. */
static void (*tick_hook)(void);

/* One tick run, `ms` after the last one. */
static void tick(uint32_t ms)
{
    fake_step(ms * 1000u);
    if (tick_hook) {
        tick_hook();
    }
    audio_tick();
}

/* Moves the clock by `us`, running the channel 3 IRQ at each due sample. */
static void run_us(uint32_t us)
{
    uint32_t end = fake_us + us;

    while ((int32_t)(end - fake_us) > 0) {
        if (fake_irq3 && (int32_t)(fake_due - fake_us) <= 0) {
            pcm_isr(IRQ_TIMER3, 0);
            continue;
        }
        if (fake_irq3 && (int32_t)(fake_due - end) < 0) {
            fake_step(fake_due - fake_us);
        } else {
            fake_step(end - fake_us);
        }
    }
}

static int fake_started;

/* Clears the PWM record and the trace output. */
static void clear_logs(void)
{
    audio_trace_flush();
    out[0] = '\0';
    pwm_log[0] = '\0';
    pwm_count = 0;
}

/* A fresh driver on `which` backend, at clock 1000 ms, in duty 0 rest mode. */
static void fresh(int which)
{
    if (!fake_started) {
        k_console_set(capture);
        CHECK_EQ(audio_select(AUDIO_BACKEND_TRACE), 0);
        CHECK_EQ(audio_init(), 0);
        fake_started = 1;
    }
    backend = which;
    k_memset((void *)&req, 0, sizeof(req));
    k_memset(&seq, 0, sizeof(seq));
    k_memset(&pcm, 0, sizeof(pcm));
    k_memset(&mix, 0, sizeof(mix));
    mix_req_set = 0;
    k_memset(&pcm_req, 0, sizeof(pcm_req));
    pcm_req_full = 0;
    pcm_full = 0;
    rest_mid = 0;
    rest_ramp = 0;
    pcm_cut = 0;
    sounding = 0;
    playing = 0;
    fake_irq3 = 0;
    tick_hook = 0;
    clock_ms = 1000;
    acc_us = 0;
    last_us = fake_us;
    audio_set_volume(100);
    clear_logs();
}

#endif
