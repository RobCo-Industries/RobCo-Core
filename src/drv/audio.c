/*
 * Audio front end: backend selection, requests from the main context and
 * playback from a tick callback.
 *
 * Ownership: the main context only writes `req` and `pcm_req` (with IRQs
 * masked). The tick callback owns the sequencer, the audio clock and the
 * backend, and applies `req` at its next run. So every change, a new tone
 * included, starts on a tick and is timed by the same clock that ends it.
 *
 * PCM: the tick starts it. In between, the system timer compare channel 3
 * IRQ outputs one sample per period and ends it. IRQ handlers do not nest,
 * so the tick and the PCM IRQ never run at the same time. A stop or a
 * replacement cuts PCM at once, with IRQs masked, so the caller owns its
 * buffer again when the call returns. The tick still prints the stop line.
 *
 * Trace rule: the tick that applies a request prints one op=stop when a
 * tone, a sequence (in a rest too) or PCM was playing, before any start
 * line. So a stop followed by a start gives the same lines whether a tick
 * runs between the two calls or not. A natural end keeps its own line.
 *
 * Queued PCM: audio_pcm_queue() chains a buffer, with IRQs masked, into
 * `pcm` when it plays or into `pcm_req` when its start waits for the tick.
 * The engine switches to it in the channel 3 IRQ, on the same time base.
 * A cut drops the waiting buffer with the playing one.
 *
 * Overlay: audio_pcm_mix() adds a second buffer to the PCM that plays, or
 * to the one that waits for its tick. The channel 3 IRQ polls a second
 * engine state, `mix`, with the same clock value, so the pure engine stays
 * as it is. The end or a cut of the play drops the overlay.
 *
 * Rest level: until audio_rest_mid() runs, silence is duty 0, as tones
 * rest. Its first call plays a short ramp up to the midpoint, and from
 * then on every silence the driver writes is the PCM range at duty 128.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/audio.h>
#include <robco/board.h>
#include <robco/crc32.h>
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

/* System timer compare channel for PCM: 0 and 2 belong to the GPU, 1 to the tick. */
#define PCM_CHANNEL             3

/* Missed compares in a row before the PCM IRQ arms with timer_oneshot_arm(). */
#define PCM_ARM_TRIES           4

/* Longest wait for the first tick in audio_init(): one period at TICK_HZ_MIN plus margin. */
#define TICK_WAIT_US            1100000u
/*
 * Reads in a row with no change before audio_init() calls the system
 * timer stopped. A running 1 MHz counter changes every microsecond, and a
 * read takes far more than a picosecond, so a million equal reads only
 * happen when the counter does not run.
 */
#define TIMER_STALL_POLLS       1000000u

/* Backends, in audio_pwm.c and audio_trace.c. */
int audio_pwm_probe(void);
int audio_pwm_setup(int pin0, unsigned fn0, int pin1, unsigned fn1);
void audio_pwm_out(uint32_t hz, unsigned vol);
void audio_pwm_pcm_mode(void);
void audio_pwm_pcm_mode_at(uint32_t dat);
void audio_pwm_sample(uint32_t dat);
void audio_trace_out(uint32_t hz, unsigned vol, uint32_t t_ms);
void audio_trace_pcm(uint32_t rate, uint32_t len, unsigned vol, uint32_t crc, uint32_t t_ms);
void audio_trace_pcm_end(uint32_t n, uint32_t last, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_pcm_next(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_pcm_mix(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_flush(void);

/* The rest ramp: duty 0 to 128 on 3t^2 - 2t^3 over this many samples at this rate. */
#define RAMP_LEN                882u
#define RAMP_RATE               22050u

static int backend = -1;            /* AUDIO_BACKEND_PWM or _TRACE once initialised */
static int forced = ROBCO_AUDIO_BACKEND;
static int probe = AUDIO_PROBE_SKIPPED;
static int registered;
static int pcm_irq;                 /* the channel 3 handler is ours */
static int pins_set;
static int pin0, pin1;
static unsigned fn0, fn1;
static volatile unsigned volume = ROBCO_AUDIO_VOLUME;

/* Request from the main context, applied by the next tick. */
static volatile struct {
    int valid;
    int stop;
    int pcm;                /* start pcm_req */
    const audio_note_t *notes;
    size_t count;
    unsigned flags;
    audio_note_t tone;      /* used when notes is NULL */
} req;
static volatile int playing;

/* PCM request, prepared by the main context with IRQs masked. */
static audio_pcm_t pcm_req;
static uint32_t pcm_req_crc;

/* CRC of the buffer queued with audio_pcm_queue(), for its trace line. IRQs masked. */
static uint32_t pcm_next_crc;

/* The play in pcm_req ignores the volume: the rest ramp. */
static int pcm_req_full;

/* Overlay for a play that waits for its tick, prepared with IRQs masked. */
static audio_pcm_t mix_req;
static uint32_t mix_req_crc;
static int mix_req_set;

/* Set by the first audio_rest_mid(): silence is the midpoint from then on. */
static volatile int rest_mid;
static uint8_t ramp[RAMP_LEN];

/* Tick context only. */
static audio_seq_t seq;
static audio_note_t cur_tone;
static uint32_t clock_ms;
static uint32_t last_us;
static uint32_t acc_us;
static uint32_t sounding;

/* IRQ context only: the tick starts and halts, the channel 3 IRQ runs. */
static audio_pcm_t pcm;
static uint32_t pcm_t0_ms;
static uint64_t pcm_t0_us;
static int pcm_cut;                 /* cut by a call: the tick still halts it */
static uint32_t pcm_total;          /* samples output since audio_init() */
static int pcm_full;                /* the play ignores the volume */
static audio_pcm_t mix;             /* the overlay, on the play's clock */
static uint8_t mix_val = 128;       /* the overlay sample that sounds now */

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

/* IRQ context, or IRQs masked. Silence on PWM: duty 0, or the midpoint in rest mode. */
static void pwm_silence(void)
{
    if (rest_mid) {
        audio_pwm_pcm_mode();
    } else {
        audio_pwm_out(0, 0);
    }
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
        if (rest_mid && (hz == 0 || vol == 0)) {
            audio_pwm_pcm_mode();
        } else {
            audio_pwm_out(hz, vol);
        }
    } else {
        audio_trace_out(hz, vol, t_ms);
    }
}

/* IRQ context. The overlay sample due at `now`, held between its samples. */
static uint8_t mix_at(uint32_t now)
{
    uint32_t pos;
    int r;

    if (!mix.active) {
        return 128;
    }
    r = audio_pcm_poll(&mix, now, &pos);
    if (r == AUDIO_PCM_SAMPLE) {
        mix_val = mix.data[pos];
    } else if (r == AUDIO_PCM_END) {
        mix_val = 128;
    }
    return mix_val;
}

/* Play sample plus overlay sample around the midpoint, clamped to 0 to 255. */
static uint8_t mix_add(uint8_t a, uint8_t b)
{
    int v = (int)a + (int)b - 128;

    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* IRQ context. One sample, with the overlay, at the current volume. */
static void pcm_out(uint32_t pos, uint32_t now)
{
    uint8_t s = mix_add(pcm.data[pos], mix_at(now));

    pcm_total++;
    if (backend == AUDIO_BACKEND_PWM) {
        audio_pwm_sample(audio_pcm_duty(audio_pcm_scale(s, pcm_full ? 100 : volume),
                                        AUDIO_PCM_PWM_RANGE));
    }
}

/* IRQ context, or IRQs masked. Drops the overlay. */
static void mix_drop(void)
{
    mix.active = 0;
    mix_val = 128;
}

/* IRQ context. The last sample has played: silence, no more IRQs. */
static void pcm_finish(void)
{
    irq_disable(IRQ_TIMER3);
    mix_drop();
    if (backend == AUDIO_BACKEND_PWM) {
        pwm_silence();
    } else {
        audio_trace_pcm_end(pcm.n, pcm.last, pcm_t0_ms, timer_now_us() - pcm_t0_us);
    }
    playing = seq.active;
}

/*
 * IRQ context. Outputs what the clock says is due, then arms the compare
 * for the next sample. A compare already in the past would not match for
 * 71 minutes, so a miss polls again at the new time. After a few misses,
 * timer_oneshot_arm() picks a time that cannot be missed.
 */
static void pcm_run(uint32_t now)
{
    unsigned tries = 0;
    uint32_t pos;
    uint32_t next;
    int r;

    for (;;) {
        r = audio_pcm_poll(&pcm, now, &pos);
        if (pcm.switches) {
            pcm.switches = 0;
            if (backend != AUDIO_BACKEND_PWM) {
                audio_trace_pcm_next(pcm.len, pcm_next_crc, pcm_t0_ms,
                                     timer_now_us() - pcm_t0_us);
            }
        }
        if (r == AUDIO_PCM_END) {
            pcm_finish();
            return;
        }
        if (r == AUDIO_PCM_SAMPLE) {
            pcm_out(pos, now);
        }
        next = audio_pcm_next_us(&pcm);
        if (++tries >= PCM_ARM_TRIES) {
            timer_oneshot_arm(PCM_CHANNEL, next);
            return;
        }
        if (!timer_compare_arm(PCM_CHANNEL, next)) {
            return;
        }
        now = timer_now_lo();
    }
}

/* Channel 3 IRQ. */
static void pcm_isr(unsigned irq, void *arg)
{
    (void)irq;
    (void)arg;

    timer_compare_clear(PCM_CHANNEL);
    if (!pcm.active) {
        irq_disable(IRQ_TIMER3);
        return;
    }
    pcm_run(timer_now_lo());
}

/* Tick context. Starts pcm_req: sample 0 plays now. */
static void pcm_begin(void)
{
    unsigned vol = volume;

    pcm = pcm_req;
    pcm_full = pcm_req_full;
    if (backend == AUDIO_BACKEND_PWM) {
        if (pcm_full) {
            /* The ramp starts from its first sample, not from the midpoint. */
            audio_pwm_pcm_mode_at(audio_pcm_duty(pcm.data[0], AUDIO_PCM_PWM_RANGE));
        } else {
            audio_pwm_pcm_mode();
        }
    } else {
        audio_trace_pcm(pcm.rate, pcm.len, pcm_full ? 100 : vol, pcm_req_crc, clock_ms);
    }
    pcm_t0_ms = clock_ms;
    pcm_t0_us = timer_now_us();
    pcm.origin_us = (uint32_t)pcm_t0_us;
    mix_drop();
    if (mix_req_set) {
        /* The overlay waited with the play: both start on this sample. */
        mix = mix_req;
        mix.origin_us = pcm.origin_us;
        mix_req_set = 0;
        if (backend != AUDIO_BACKEND_PWM) {
            audio_trace_pcm_mix(mix.len, mix_req_crc, clock_ms, 0);
        }
    }
    timer_compare_clear(PCM_CHANNEL);
    irq_enable(IRQ_TIMER3);
    pcm_run(pcm.origin_us);
}

/*
 * Any context, IRQs masked. Stops reading the PCM buffer now: no more
 * samples, silence on PWM. A compare that matched while IRQs were masked
 * is cleared here, and pcm_isr() ignores one that still fires. The tick
 * prints the stop record later through pcm_halt().
 */
static void pcm_cut_locked(void)
{
    /* An overlay that waits goes with the request it waits on. */
    mix_req_set = 0;
    if (!pcm.active) {
        return;
    }
    pcm.active = 0;
    pcm.next = 0;
    pcm_cut = 1;
    mix_drop();
    irq_disable(IRQ_TIMER3);
    timer_compare_clear(PCM_CHANNEL);
    if (backend == AUDIO_BACKEND_PWM) {
        pwm_silence();
    }
}

/* Tick context. Stops PCM mid-play: silence, no more IRQs. The caller traces the stop. */
static void pcm_halt(void)
{
    pcm.active = 0;
    pcm.next = 0;
    pcm_cut = 0;
    mix_drop();
    irq_disable(IRQ_TIMER3);
    timer_compare_clear(PCM_CHANNEL);
    if (backend == AUDIO_BACKEND_PWM) {
        pwm_silence();
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
        /* A started play ends here, by a stop or a replacement: one stop line. */
        int cut = seq.active || pcm.active || pcm_cut;

        if (pcm.active || pcm_cut) {
            pcm_halt();
        }
        if (cut && backend != AUDIO_BACKEND_PWM) {
            audio_trace_out(0, volume, clock_ms);
            /* Traced already: output() prints only a new tone. PWM keeps its writes. */
            sounding = 0;
        }
        if (req.stop) {
            audio_seq_stop(&seq);
        } else if (req.pcm) {
            audio_seq_stop(&seq);
            sounding = 0;
            pcm_begin();
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
    playing = seq.active || pcm.active;
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
    uint32_t last;
    uint32_t same = 0;
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
    last = t0;
    while (tick_count() == c0) {
        uint32_t now = timer_now_lo();

        if (now - t0 >= TICK_WAIT_US) {
            return AUDIO_ERR_NO_TICK;
        }
        if (now != last) {
            last = now;
            same = 0;
        } else if (++same >= TIMER_STALL_POLLS) {
            return AUDIO_ERR_TIMER;
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
    if (!pcm_irq && irq_register(IRQ_TIMER3, pcm_isr, 0) == 0) {
        pcm_irq = 1;
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
    pcm_cut_locked();
    req.stop = 0;
    req.pcm = 0;
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
    pcm_cut_locked();
    req.stop = 1;
    req.valid = 1;
    irq_restore(s);
}

uint32_t audio_pcm_samples(uint32_t *play)
{
    uint32_t s;
    uint32_t total;

    s = irq_save();
    total = pcm_total;
    if (play) {
        *play = pcm.n;
    }
    irq_restore(s);
    return total;
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

int audio_play_pcm(const uint8_t *data, size_t len, uint32_t rate_hz, unsigned flags)
{
    audio_pcm_t p;
    uint32_t crc = 0;
    uint32_t s;
    int ret;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    ret = audio_pcm_check(data, len, rate_hz, flags);
    if (ret) {
        return ret;
    }
    if (!pcm_irq) {
        return AUDIO_ERR_NO_IRQ;
    }
    /*
     * Fail fast when busy, before the CRC scans the whole buffer. The
     * check repeats with IRQs masked below: a sound can start in between.
     */
    if (!(flags & AUDIO_PRIORITY)) {
        s = irq_save();
        ret = sound_pending_locked();
        irq_restore(s);
        if (ret) {
            return AUDIO_ERR_BUSY;
        }
    }
    /* The 64-bit divides of the start run here, not in an IRQ handler. */
    audio_pcm_start(&p, data, len, rate_hz, flags, 0);
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(data, len);
    }
    s = irq_save();
    if (!(flags & AUDIO_PRIORITY) && sound_pending_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    pcm_cut_locked();
    pcm_req = p;
    pcm_req_crc = crc;
    pcm_req_full = 0;
    req.stop = 0;
    req.pcm = 1;
    req.notes = 0;
    req.valid = 1;
    irq_restore(s);
    return 0;
}

/* The play a queued buffer would join: the pending start, or the one that plays. IRQs masked. */
static audio_pcm_t *queue_target_locked(void)
{
    if (req.valid) {
        return req.pcm && !req.stop ? &pcm_req : 0;
    }
    return pcm.active ? &pcm : 0;
}

/* 0 when a buffer can be queued now, else AUDIO_ERR_STATE or AUDIO_ERR_BUSY. IRQs masked. */
static int queue_check_locked(void)
{
    audio_pcm_t *t = queue_target_locked();

    if (!t || (t->flags & AUDIO_LOOP)) {
        return AUDIO_ERR_STATE;
    }
    return t->next ? AUDIO_ERR_BUSY : 0;
}

int audio_pcm_queue(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    uint32_t s;
    int ret;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    /* Same pointer and length rules as audio_play_pcm(); any valid rate will do. */
    ret = audio_pcm_check(data, len, AUDIO_PCM_RATE_MIN, 0);
    if (ret) {
        return ret;
    }
    /* Fail fast before the CRC scans the buffer. The check repeats below. */
    s = irq_save();
    ret = queue_check_locked();
    irq_restore(s);
    if (ret) {
        return ret;
    }
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(data, len);
    }
    s = irq_save();
    ret = queue_check_locked();
    if (!ret) {
        audio_pcm_chain(queue_target_locked(), data, len);
        pcm_next_crc = crc;
    }
    irq_restore(s);
    return ret;
}

int audio_pcm_can_queue(void)
{
    uint32_t s;
    int ok;

    s = irq_save();
    ok = queue_check_locked() == 0;
    irq_restore(s);
    return ok;
}

/*
 * The play an overlay would join, as for queue_target_locked(). Sets
 * *pending when that play still waits for its tick. IRQs masked.
 */
static audio_pcm_t *mix_target_locked(int *pending)
{
    if (req.valid) {
        *pending = 1;
        return req.pcm && !req.stop ? &pcm_req : 0;
    }
    *pending = 0;
    return pcm.active ? &pcm : 0;
}

/* 0 when an overlay can start now, else AUDIO_ERR_STATE or AUDIO_ERR_BUSY. IRQs masked. */
static int mix_check_locked(void)
{
    int pending;

    if (!mix_target_locked(&pending)) {
        return AUDIO_ERR_STATE;
    }
    return (pending ? mix_req_set : mix.active) ? AUDIO_ERR_BUSY : 0;
}

int audio_pcm_mix(const uint8_t *data, size_t len)
{
    audio_pcm_t m;
    audio_pcm_t *t;
    uint32_t crc = 0;
    uint32_t rate;
    uint32_t s;
    int pending;
    int ret;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    ret = audio_pcm_check(data, len, AUDIO_PCM_RATE_MIN, 0);
    if (ret) {
        return ret;
    }
    /* Fail fast before the CRC scans the buffer. The check repeats below. */
    s = irq_save();
    ret = mix_check_locked();
    rate = ret ? 0 : mix_target_locked(&pending)->rate;
    irq_restore(s);
    if (ret) {
        return ret;
    }
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(data, len);
    }
    /* The 64-bit divides of the start run here. The rate can change below: then redo them. */
    audio_pcm_start(&m, data, len, rate, 0, 0);
    s = irq_save();
    ret = mix_check_locked();
    if (ret) {
        irq_restore(s);
        return ret;
    }
    t = mix_target_locked(&pending);
    if (t->rate != rate) {
        audio_pcm_start(&m, data, len, t->rate, 0, 0);
    }
    if (pending) {
        mix_req = m;
        mix_req_crc = crc;
        mix_req_set = 1;
    } else {
        /* From the next sample of the play, on the same grid. */
        m.origin_us = audio_pcm_next_us(&pcm);
        mix = m;
        mix_val = 128;
        if (backend != AUDIO_BACKEND_PWM) {
            audio_trace_pcm_mix(m.len, crc, pcm_t0_ms,
                                (uint32_t)(m.origin_us - (uint32_t)pcm_t0_us));
        }
    }
    irq_restore(s);
    return 0;
}

int audio_pcm_mix_busy(void)
{
    uint32_t s;
    int b;

    s = irq_save();
    b = mix_req_set || mix.active;
    irq_restore(s);
    return b;
}

/* 0 to 128 on 3t^2 - 2t^3, t = i / (RAMP_LEN - 1), rounded to the nearest step. */
static void ramp_fill(void)
{
    const uint64_t n = RAMP_LEN - 1;
    const uint64_t n3 = n * n * n;
    uint64_t i;

    for (i = 0; i < RAMP_LEN; i++) {
        uint64_t f = 3 * i * i * n - 2 * i * i * i;

        ramp[i] = (uint8_t)((128 * f + n3 / 2) / n3);
    }
}

int audio_rest_mid(void)
{
    audio_pcm_t p;
    uint32_t crc = 0;
    uint32_t s;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    if (rest_mid) {
        return 0;
    }
    if (!pcm_irq) {
        return AUDIO_ERR_NO_IRQ;
    }
    s = irq_save();
    if (sound_pending_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    irq_restore(s);
    ramp_fill();
    audio_pcm_start(&p, ramp, RAMP_LEN, RAMP_RATE, 0, 0);
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(ramp, RAMP_LEN);
    }
    s = irq_save();
    if (rest_mid) {
        irq_restore(s);
        return 0;
    }
    if (sound_pending_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    pcm_cut_locked();
    pcm_req = p;
    pcm_req_crc = crc;
    pcm_req_full = 1;
    req.stop = 0;
    req.pcm = 1;
    req.notes = 0;
    req.valid = 1;
    rest_mid = 1;
    irq_restore(s);
    return 0;
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
    pcm_cut_locked();
    for (i = 0; i < (size_t)n; i++) {
        tune[i] = tmp[i];
    }
    req.stop = 0;
    req.pcm = 0;
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
#ifndef ROBCO_HOST
        __asm__ volatile("wfi" : : : "memory");
#endif
        irq_restore(s);
    }
}
