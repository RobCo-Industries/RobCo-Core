/*
 * Audio front end: backend selection, requests from the main context and
 * playback from a tick callback.
 *
 * Ownership: the main context only writes `req` (with IRQs masked). The
 * tick callback owns the sequencer, the audio clock and the backend, and
 * applies `req` at its next run. So every change, a new tone included,
 * starts on a tick and is timed by the same clock that ends it.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/audio.h>
#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/irq.h>
#include <robco/tick.h>
#include <robco/timer.h>

#ifndef ROBCO_AUDIO_BACKEND
#define ROBCO_AUDIO_BACKEND     AUDIO_BACKEND_AUTO
#endif
#ifndef ROBCO_AUDIO_TUNE_MAX
#define ROBCO_AUDIO_TUNE_MAX    64
#endif
#ifndef ROBCO_AUDIO_BEEP_HZ
#define ROBCO_AUDIO_BEEP_HZ     1000
#endif
#ifndef ROBCO_AUDIO_BEEP_MS
#define ROBCO_AUDIO_BEEP_MS     80
#endif
#ifndef ROBCO_AUDIO_VOLUME
#define ROBCO_AUDIO_VOLUME      100
#endif

/* Checked at build time: a bad value stops the build instead of a silent beep. */
_Static_assert(ROBCO_AUDIO_BEEP_HZ >= AUDIO_HZ_MIN && ROBCO_AUDIO_BEEP_HZ <= AUDIO_HZ_MAX,
               "ROBCO_AUDIO_BEEP_HZ must be within AUDIO_HZ_MIN to AUDIO_HZ_MAX");
_Static_assert(ROBCO_AUDIO_BEEP_MS > 0, "ROBCO_AUDIO_BEEP_MS must be above 0");
_Static_assert(ROBCO_AUDIO_VOLUME <= 100,
               "ROBCO_AUDIO_VOLUME must be 0 to 100");
#ifdef ROBCO_AUDIO_PIN0
_Static_assert(ROBCO_AUDIO_PIN0 >= 0 && ROBCO_AUDIO_PIN0 < GPIO_PIN_COUNT && ROBCO_AUDIO_FN0 <= 7,
               "ROBCO_AUDIO_PIN0 must be a GPIO pin and ROBCO_AUDIO_FN0 a GPIO_FN_* code");
#ifdef ROBCO_AUDIO_PIN1
_Static_assert(ROBCO_AUDIO_PIN1 == AUDIO_PIN_NONE ||
               (ROBCO_AUDIO_PIN1 >= 0 && ROBCO_AUDIO_PIN1 < GPIO_PIN_COUNT && ROBCO_AUDIO_FN1 <= 7),
               "ROBCO_AUDIO_PIN1 must be a GPIO pin and ROBCO_AUDIO_FN1 a GPIO_FN_* code");
#endif
#endif

/* Step changes handled one by one per tick before the rest is skipped. */
#define EVENTS_PER_TICK         16

/* Longest wait for the first tick in audio_init(): one period at TICK_HZ_MIN plus margin. */
#define TICK_WAIT_US            1100000u

/* Backends, in audio_pwm.c and audio_trace.c. */
int audio_pwm_probe(void);
int audio_pwm_setup(int pin0, unsigned fn0, int pin1, unsigned fn1);
void audio_pwm_out(uint32_t hz, unsigned vol);
void audio_trace_out(uint32_t hz, unsigned vol, uint32_t t_ms);
void audio_trace_flush(void);

static int backend = -1;            /* AUDIO_BACKEND_PWM or _TRACE once initialised */
static int forced = ROBCO_AUDIO_BACKEND;
static int probe = AUDIO_PROBE_SKIPPED;
static int registered;
static int pins_set;
static int pin0, pin1;
static unsigned fn0, fn1;
static volatile unsigned volume = ROBCO_AUDIO_VOLUME;

/* Request from the main context, applied by the next tick. */
static volatile struct {
    int valid;
    int stop;
    const audio_note_t *notes;
    size_t count;
    unsigned flags;
    audio_note_t tone;      /* used when notes is NULL */
} req;
static volatile int playing;

/* Tick context only. */
static audio_seq_t seq;
static audio_note_t cur_tone;
static uint32_t clock_ms;
static uint32_t last_us;
static uint32_t acc_us;
static uint32_t sounding;

static audio_note_t tune[ROBCO_AUDIO_TUNE_MAX];
static const audio_note_t beep = { ROBCO_AUDIO_BEEP_HZ, ROBCO_AUDIO_BEEP_MS };

/* New-style revision codes carry the board type in bits 11:4. */
static int board_is_zero(uint32_t rev)
{
    uint32_t type = (rev >> 4) & 0xff;

    if (!(rev & (1u << 23))) {
        return 0;
    }
    return type == 0x09 || type == 0x0c || type == 0x12;
}

static void default_pins(void)
{
#ifdef ROBCO_AUDIO_PIN0
    pin0 = ROBCO_AUDIO_PIN0;
    fn0 = ROBCO_AUDIO_FN0;
#ifdef ROBCO_AUDIO_PIN1
    pin1 = ROBCO_AUDIO_PIN1;
    fn1 = ROBCO_AUDIO_FN1;
#else
    pin1 = AUDIO_PIN_NONE;
    fn1 = 0;
#endif
#else
    const robco_board_t *b = board_get();

    if (board_is_zero(b->board_rev)) {
        /* No jack: PWM0 on header pin 12 (GPIO 18), mono. */
        pin0 = 18;
        fn0 = GPIO_FN_ALT5;
        pin1 = AUDIO_PIN_NONE;
        fn1 = 0;
    } else {
        pin0 = 40;
        fn0 = GPIO_FN_ALT0;
        pin1 = b->soc == ROBCO_SOC_BCM2837 ? 41 : 45;
        fn1 = GPIO_FN_ALT0;
    }
#endif
}

static void output(uint32_t t_ms)
{
    uint32_t hz = seq.hz;
    unsigned vol = volume;

    if (!hz && !sounding) {
        return;
    }
    sounding = hz;
    if (backend == AUDIO_BACKEND_PWM) {
        audio_pwm_out(hz, vol);
    } else {
        audio_trace_out(hz, vol, t_ms);
    }
}

/* IRQ context. */
static void audio_tick(void)
{
    uint32_t now = timer_now_lo();
    uint32_t at;
    int n = 0;

    acc_us += now - last_us;
    last_us = now;
    if (acc_us >= 1000) {
        clock_ms += acc_us / 1000;
        acc_us %= 1000;
    }
    if (backend < 0) {
        return;
    }
    if (req.valid) {
        if (req.stop) {
            audio_seq_stop(&seq);
        } else if (req.notes) {
            audio_seq_start(&seq, req.notes, req.count, req.flags, clock_ms);
        } else {
            cur_tone = req.tone;
            audio_seq_start(&seq, &cur_tone, 1, 0, clock_ms);
        }
        req.valid = 0;
        output(clock_ms);
    }
    while (audio_seq_poll(&seq, clock_ms, &at)) {
        if (n < EVENTS_PER_TICK) {
            output(at);
        }
        n++;
    }
    if (n > EVENTS_PER_TICK) {
        output(clock_ms);
    }
    playing = seq.active;
}

int audio_select(int which)
{
    if (backend >= 0) {
        return AUDIO_ERR_STATE;
    }
    if (which != AUDIO_BACKEND_AUTO && which != AUDIO_BACKEND_PWM &&
        which != AUDIO_BACKEND_TRACE) {
        return AUDIO_ERR_ARG;
    }
    forced = which;
    return 0;
}

static int pin_ok(int pin, unsigned fn)
{
    return pin == AUDIO_PIN_NONE || (pin >= 0 && pin < GPIO_PIN_COUNT && fn <= 7);
}

int audio_set_pins(int p0, unsigned f0, int p1, unsigned f1)
{
    if (backend >= 0) {
        return AUDIO_ERR_STATE;
    }
    if (!pin_ok(p0, f0) || !pin_ok(p1, f1)) {
        return AUDIO_ERR_ARG;
    }
    pin0 = p0;
    fn0 = f0;
    pin1 = p1;
    fn1 = f1;
    pins_set = 1;
    return 0;
}

void audio_get_pins(int *p0, unsigned *f0, int *p1, unsigned *f1)
{
    if (!pins_set) {
        default_pins();
        pins_set = 1;
    }
    if (p0) {
        *p0 = pin0;
    }
    if (f0) {
        *f0 = fn0;
    }
    if (p1) {
        *p1 = pin1;
    }
    if (f1) {
        *f1 = fn1;
    }
}

int audio_init(void)
{
    uint32_t s;
    uint32_t t0;
    uint32_t c0;
    int ret = 0;

    if (backend >= 0) {
        return 0;
    }
    if (!irq_ready()) {
        return AUDIO_ERR_NO_IRQ;
    }
    s = irq_save();
    irq_restore(s);
    if (s) {
        return AUDIO_ERR_NO_TICK;
    }
    /* The tick must advance. At TICK_HZ_MIN this can take a second. */
    c0 = tick_count();
    t0 = timer_now_lo();
    while (tick_count() == c0) {
        if (timer_now_lo() - t0 >= TICK_WAIT_US) {
            return AUDIO_ERR_NO_TICK;
        }
    }
    if (!registered) {
        s = irq_save();
        last_us = timer_now_lo();
        acc_us = 0;
        irq_restore(s);
        clock_ms = tick_ms();
        if (tick_register(audio_tick)) {
            return AUDIO_ERR_TICK_FULL;
        }
        registered = 1;
    }
    audio_get_pins(0, 0, 0, 0);

    if (forced == AUDIO_BACKEND_TRACE) {
        probe = AUDIO_PROBE_SKIPPED;
    } else {
        probe = audio_pwm_probe();
    }
    if (forced == AUDIO_BACKEND_PWM && probe == AUDIO_PROBE_ABORT) {
        /* Registers that abort cannot be driven. Stay usable on trace. */
        ret = AUDIO_ERR_NO_PWM;
    }
    if ((probe == AUDIO_PROBE_OK ||
         (forced == AUDIO_BACKEND_PWM && probe == AUDIO_PROBE_IGNORED)) &&
        audio_pwm_setup(pin0, fn0, pin1, fn1) != 0) {
        /* A pin did not take its PWM function. Stay usable on trace. */
        ret = AUDIO_ERR_PIN;
        s = irq_save();
        backend = AUDIO_BACKEND_TRACE;
        irq_restore(s);
    } else if (probe == AUDIO_PROBE_OK ||
               (forced == AUDIO_BACKEND_PWM && probe == AUDIO_PROBE_IGNORED)) {
        s = irq_save();
        backend = AUDIO_BACKEND_PWM;
        irq_restore(s);
    } else {
        s = irq_save();
        backend = AUDIO_BACKEND_TRACE;
        irq_restore(s);
    }
    return ret;
}

const char *audio_backend(void)
{
    if (backend == AUDIO_BACKEND_PWM) {
        return "pwm";
    }
    if (backend == AUDIO_BACKEND_TRACE) {
        return "trace";
    }
    return "none";
}

int audio_probe_result(void)
{
    return probe;
}

const char *audio_probe_name(int result)
{
    switch (result) {
    case AUDIO_PROBE_OK:      return "ok";
    case AUDIO_PROBE_ABORT:   return "abort";
    case AUDIO_PROBE_IGNORED: return "ignored";
    default:                  return "skipped";
    }
}

/* Something plays, or a request waits for the tick. IRQs masked. */
static int busy_locked(void)
{
    return req.valid || playing;
}

/* Something plays or will play: a pending stop does not count. IRQs masked. */
static int sound_pending_locked(void)
{
    return req.valid ? !req.stop : playing;
}

/* Queues a request. `tone` is used when notes is NULL. */
static int submit(const audio_note_t *notes, size_t count, unsigned flags,
                  uint32_t hz, uint32_t ms, int replace)
{
    uint32_t s;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    s = irq_save();
    if (!replace && sound_pending_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    req.stop = 0;
    req.notes = notes;
    req.count = count;
    req.flags = flags;
    req.tone.hz = hz;
    req.tone.ms = ms;
    req.valid = 1;
    irq_restore(s);
    return 0;
}

static int hz_ok(uint32_t hz)
{
    return hz == 0 || (hz >= AUDIO_HZ_MIN && hz <= AUDIO_HZ_MAX);
}

int audio_tone(uint32_t hz, uint32_t ms)
{
    if (!hz_ok(hz)) {
        return AUDIO_ERR_RANGE;
    }
    if (ms == 0) {
        return backend < 0 ? AUDIO_ERR_NO_INIT : 0;
    }
    return submit(0, 0, 0, hz, ms, 1);
}

int audio_beep(void)
{
    return submit(&beep, 1, 0, 0, 0, 1);
}

void audio_stop(void)
{
    uint32_t s;

    if (backend < 0) {
        return;
    }
    s = irq_save();
    req.stop = 1;
    req.valid = 1;
    irq_restore(s);
}

int audio_busy(void)
{
    uint32_t s;
    int b;

    s = irq_save();
    b = busy_locked();
    irq_restore(s);
    return b;
}

void audio_set_volume(unsigned percent)
{
    volume = percent > 100 ? 100 : percent;
}

int audio_play(const audio_note_t *notes, size_t count, unsigned flags)
{
    size_t i;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    if (count && !notes) {
        return AUDIO_ERR_ARG;
    }
    if (flags & ~(AUDIO_LOOP | AUDIO_PRIORITY)) {
        return AUDIO_ERR_ARG;
    }
    for (i = 0; i < count; i++) {
        if (!hz_ok(notes[i].hz)) {
            return AUDIO_ERR_RANGE;
        }
    }
    if (count == 0) {
        return 0;
    }
    return submit(notes, count, flags & AUDIO_LOOP, 0, 0, flags & AUDIO_PRIORITY);
}

int audio_play_rtttl(const char *text, unsigned flags)
{
    audio_note_t tmp[ROBCO_AUDIO_TUNE_MAX];
    uint32_t s;
    int n;
    size_t i;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    if (flags & ~(AUDIO_LOOP | AUDIO_PRIORITY)) {
        return AUDIO_ERR_ARG;
    }
    n = audio_rtttl_parse(text, tmp, ROBCO_AUDIO_TUNE_MAX, 0);
    if (n <= 0) {
        return n;
    }
    /*
     * The tick reads `tune` only through the sequencer, and a pending
     * request replaces the sequencer before the next poll. So the copy is
     * safe once the request is in, all with IRQs masked.
     */
    s = irq_save();
    if (!(flags & AUDIO_PRIORITY) && sound_pending_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    for (i = 0; i < (size_t)n; i++) {
        tune[i] = tmp[i];
    }
    req.stop = 0;
    req.notes = tune;
    req.count = (size_t)n;
    req.flags = flags & AUDIO_LOOP;
    req.valid = 1;
    irq_restore(s);
    return 0;
}

void audio_poll(void)
{
    audio_trace_flush();
}

void audio_wait(void)
{
    uint32_t s;

    for (;;) {
        audio_poll();
        s = irq_save();
        if (s) {
            /* IRQs were masked by the caller: the tick cannot run. */
            irq_restore(s);
            return;
        }
        if (backend < 0 || !busy_locked()) {
            irq_restore(s);
            audio_poll();
            return;
        }
        /* wfi wakes on a pending IRQ even while masked; the handler runs after restore. */
        __asm__ volatile("wfi" : : : "memory");
        irq_restore(s);
    }
}
