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
 * to the one that waits for its tick. It has no clock of its own: it
 * stores the play's grid sample `at` where its byte 0 sounds, and each
 * play sample with grid sample `at + k` sounds byte k. So the overlay and
 * the play share one grid by construction. The end or a cut of the play
 * drops the overlay. A play with no sample left takes no overlay.
 *
 * Rest level: silence is duty 0, as tones rest, until a ramp from
 * audio_rest_mid() reaches its last sample. The ramp never moves the duty
 * by more than its largest step, so a late IRQ cannot make the pin jump.
 * From then on every silence the
 * driver writes is the PCM range at duty 128. A ramp that ends early, by a
 * stop, a cut or a new sound, pending or playing, leaves the rest level at
 * duty 0 and writes that silence, and a later call can try again.
 *
 * Centred tones: once the rest level is the midpoint, a tone is a square
 * around 128 made in the sample IRQ, a play of kind tone on the PCM
 * engine. The tick starts it when a sequence first sounds and halts it in
 * the tick where the sequence ends. Each note restarts the square on the
 * same grid, and a rest writes 128. The trace lines stay those of a tone.
 * A note after a sounding note keeps the gain and its move. A note after
 * a rest starts at the target, as a play does.
 * Before the ramp ends, a tone stays the PWM's own pulse.
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
/* Height in steps of a tone at volume 100 once the rest is the midpoint. */
#ifndef ROBCO_AUDIO_TONE_PEAK
#define ROBCO_AUDIO_TONE_PEAK   127
#endif
/* Sample grid of a tone once the rest is the midpoint. */
#ifndef ROBCO_AUDIO_TONE_RATE
#define ROBCO_AUDIO_TONE_RATE   37500
#endif

/* Checked at build time: a bad value stops the build instead of a silent beep. */
_Static_assert(ROBCO_AUDIO_BEEP_HZ >= AUDIO_HZ_MIN && ROBCO_AUDIO_BEEP_HZ <= AUDIO_HZ_MAX,
               "ROBCO_AUDIO_BEEP_HZ must be within AUDIO_HZ_MIN to AUDIO_HZ_MAX");
_Static_assert(ROBCO_AUDIO_BEEP_MS > 0, "ROBCO_AUDIO_BEEP_MS must be above 0");
_Static_assert(ROBCO_AUDIO_VOLUME <= 100,
               "ROBCO_AUDIO_VOLUME must be 0 to 100");
_Static_assert(ROBCO_AUDIO_TONE_PEAK >= 0 && ROBCO_AUDIO_TONE_PEAK <= 127,
               "ROBCO_AUDIO_TONE_PEAK must be 0 to 127");
_Static_assert(ROBCO_AUDIO_TONE_RATE >= AUDIO_PCM_RATE_MIN &&
               ROBCO_AUDIO_TONE_RATE <= AUDIO_PCM_RATE_MAX,
               "ROBCO_AUDIO_TONE_RATE must be within AUDIO_PCM_RATE_MIN to AUDIO_PCM_RATE_MAX");
/*
 * Floor of the tone grid. A centred tone plays at most half its grid, see
 * audio_square.c. The highest note of the note table, B8 from
 * audio_note_hz(11, 8), is 7902 Hz, so a grid of 15804 Hz or more plays
 * every note of a sequence at its own pitch. 16000 rounds that up and
 * stays under 18750, the lighter grid the guide offers. A plain tone
 * above half the grid still plays, at half the grid.
 */
#define AUDIO_TONE_RATE_FLOOR   16000
_Static_assert(ROBCO_AUDIO_TONE_RATE >= AUDIO_TONE_RATE_FLOOR,
               "ROBCO_AUDIO_TONE_RATE must be 16000 or more: the highest note, 7902 Hz, needs twice its pitch");
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
void audio_trace_out(uint32_t hz, uint32_t gain, uint32_t t_ms);
void audio_trace_pcm(uint32_t rate, uint32_t len, uint32_t gain, uint32_t crc, uint32_t t_ms,
                     unsigned loop);
void audio_trace_pcm_end(uint32_t n, uint32_t last, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_pcm_next(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_pcm_mix(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_voice(uint32_t rate, uint32_t spans, uint32_t len, uint32_t gain, uint32_t crc,
                       uint32_t t_ms);
void audio_trace_gain(uint32_t gain, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_flush(void);

/* The rest ramp: duty 0 to 128 on 3t^2 - 2t^3 over this many samples at this rate. */
#define RAMP_LEN                882u
#define RAMP_RATE               22050u
/* One ramp sample period, rounded up: the wait after a late ramp step. */
#define RAMP_STEP_US            ((1000000u + RAMP_RATE - 1) / RAMP_RATE)

static int backend = -1;            /* AUDIO_BACKEND_PWM or _TRACE once initialised */
static int forced = ROBCO_AUDIO_BACKEND;
static int probe = AUDIO_PROBE_SKIPPED;
static int registered;
static int pcm_irq;                 /* the channel 3 handler is ours */
static int pins_set;
static int pin0, pin1;
static unsigned fn0, fn1;
/*
 * Gain, in units of 1/10000. The main context writes the target. The
 * play's level is gain_cur: a play starts at the target, and the sample
 * IRQ moves it toward the target by gain_step per grid sample. The gain
 * follows the sample grid, so a late sample catches up. gain_n is the
 * grid sample up to which gain_cur has moved, kept even while the gain
 * rests. It never goes back. The IRQ context writes gain_cur, gain_step
 * and gain_n. audio_set_gain() writes gain_cur and gain_n too, with IRQs
 * masked: a change starts at the grid sample of the call.
 */
static volatile uint32_t gain_target = ROBCO_AUDIO_VOLUME * 100u;
static uint32_t gain_cur = ROBCO_AUDIO_VOLUME * 100u;
static uint32_t gain_step;
static uint32_t gain_n;
/* Slew step of a centred tone, on its fixed grid. */
#define TONE_GAIN_STEP          ((AUDIO_GAIN_SLEW + ROBCO_AUDIO_TONE_RATE - 1u) / ROBCO_AUDIO_TONE_RATE)

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

/*
 * Play kinds on the PCM engine. A sample play reads its buffer. The ramp
 * reads its own table at full scale. A voice computes each sample from
 * its spans and reads no buffer.
 */
#define KIND_SAMPLE             0
#define KIND_RAMP               1
#define KIND_VOICE              2
#define KIND_TONE               3

/* The kind of the play in pcm_req. IRQs masked. */
static int pcm_req_kind;
/* The voice in pcm_req, when pcm_req_kind is KIND_VOICE. IRQs masked. */
static audio_voice_t voice_req;

/* An overlay: byte k sounds with the play's grid sample at + k. */
typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t at;
    int active;
} mix_t;

/* Overlay for a play that waits for its tick, prepared with IRQs masked. */
static mix_t mix_req;
static uint32_t mix_req_crc;
static int mix_req_set;

/* Set when the ramp reaches its end: silence is the midpoint from then on. */
static volatile int rest_mid;
/* A ramp from audio_rest_mid() waits for its tick or plays. IRQs masked. */
static volatile int rest_ramp;
static uint8_t ramp[RAMP_LEN];
/* IRQ context: the next ramp index to write. */
static uint32_t ramp_i;
/* The ramp's largest step between two indexes, set by ramp_fill(). */
static uint32_t ramp_step;

/*
 * Tick context only. Widths: clock_ms and every t_ms wrap at 2^32 ms, 49
 * days, as tick_ms() does, and the trace prints them modulo 2^32. last_us
 * and acc_us hold one tick period plus a remainder under 1000, far below
 * 2^32 us.
 */
static audio_seq_t seq;
static audio_note_t cur_tone;
static uint32_t clock_ms;
static uint32_t last_us;
static uint32_t acc_us;
static uint32_t sounding;
/*
 * The t_ms of the last op=gain of the tone that plays, and 1 in
 * tone_mark_set while it holds one. The tone's own lines never print a
 * time before it. Written with IRQs masked or in the tick.
 */
static uint32_t tone_mark_ms;
static int tone_mark_set;

/*
 * IRQ context only: the tick starts and halts, the channel 3 IRQ runs.
 * pcm_t0_us is 64 bits: every duration since the start comes from it, so a
 * looped play can pass the 32-bit wrap. pcm_total and pcm.n count samples
 * modulo 2^32: they wrap after 31 hours at 37500 Hz, which the trace and
 * audio_pcm_samples() report as is.
 */
static audio_pcm_t pcm;
static uint32_t pcm_t0_ms;
static uint64_t pcm_t0_us;
static int pcm_cut;                 /* cut by a call: the tick still halts it */
static uint32_t pcm_total;          /* samples output since audio_init() */
static int pcm_kind;                /* KIND_* of the play */
static audio_voice_t voice;         /* the play's spans, when it is a voice */
static mix_t mix;                   /* the overlay, on the play's grid */
static audio_square_t square;       /* the note, when the play is a tone */

/* A tone play with sample 0 at time 0, made in the main context by audio_rest_mid(). */
static audio_pcm_t tone_pcm;

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

static void tone_note(uint32_t hz, uint32_t was);

/*
 * Tick context. The time a tone line prints: never before the last
 * op=gain of the tone. A gain line takes the timer at the call, a tick
 * line the time of its tick or its event, so the two can cross by up to
 * a millisecond, or by the lateness of a late tick.
 */
static uint32_t tone_t(uint32_t t_ms)
{
    if (tone_mark_set && (int32_t)(t_ms - tone_mark_ms) < 0) {
        return tone_mark_ms;
    }
    return t_ms;
}

static void output(uint32_t t_ms)
{
    uint32_t hz = seq.hz;
    uint32_t g = gain_target;
    uint32_t was = sounding;

    if (!hz && !sounding) {
        return;
    }
    sounding = hz;
    t_ms = tone_t(t_ms);
    if (rest_mid) {
        tone_note(hz, was);
        if (backend != AUDIO_BACKEND_PWM) {
            audio_trace_out(hz, g, t_ms);
        }
        return;
    }
    if (backend == AUDIO_BACKEND_PWM) {
        /* A pulse takes the gain per note: no sample IRQ to slew it. */
        audio_pwm_out(hz, audio_gain_pulse(g));
    } else {
        audio_trace_out(hz, g, t_ms);
    }
}

/*
 * IRQ context. The overlay byte for the play sample just polled: byte k
 * goes with grid sample at + k. Before `at` it adds nothing; past its last
 * byte it ends. A play sample the clock skips skips its overlay byte too.
 */
static uint8_t mix_at(void)
{
    uint32_t k;

    if (!mix.active) {
        return 128;
    }
    k = pcm.last_n - mix.at;
    if ((int32_t)k < 0) {
        return 128;
    }
    if (k >= mix.len) {
        mix.active = 0;
        return 128;
    }
    return mix.data[k];
}

/* Play sample plus overlay sample around the midpoint, clamped to 0 to 255. */
static uint8_t mix_add(uint8_t a, uint8_t b)
{
    int v = (int)a + (int)b - 128;

    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/*
 * IRQ context. One sample, with the overlay, at the play's gain. The gain
 * first moves toward its target by one step per grid sample since its
 * last move, an output or a change, so a late sample catches up. A sample play adds two bytes,
 * clamps and scales. A voice adds the overlay to its value on 1/256 of a
 * step and rounds once, after the gain. A tone takes its height from the
 * gain at each sample. The ramp plays at full scale. The voice runs on
 * both backends, so the trace backend in QEMU pays the same cost per
 * sample as PWM.
 */
static void pcm_out(uint32_t pos)
{
    uint32_t g = gain_target;
    uint8_t s;

    if (pcm_kind != KIND_RAMP) {
        /* Kept at rest too: a change counts only the samples after it. */
        audio_gain_follow(&gain_cur, &gain_n, g, gain_step, pcm.last_n);
    }
    if (pcm_kind == KIND_TONE) {
        /* No overlay. */
        s = audio_square_out(audio_square_acc(&square, pcm.last_n),
                             (int32_t)audio_square_amp_g(ROBCO_AUDIO_TONE_PEAK, gain_cur));
    } else if (pcm_kind == KIND_VOICE) {
        int32_t q = audio_voice_at(&voice, pos);

        s = audio_voice_out_g(q, mix_at(), gain_cur);
    } else if (pcm_kind == KIND_RAMP) {
        s = mix_add(pcm.data[pos], mix_at());
    } else {
        s = audio_pcm_scale_g(mix_add(pcm.data[pos], mix_at()), gain_cur);
    }
    pcm_total++;
    if (backend == AUDIO_BACKEND_PWM) {
        audio_pwm_sample(audio_pcm_duty(s, AUDIO_PCM_PWM_RANGE));
    }
}

/* IRQ context, or IRQs masked. Drops the overlay. */
static void mix_drop(void)
{
    mix.active = 0;
}

/* IRQ context. The last sample has played: silence, no more IRQs. */
static void pcm_finish(void)
{
    irq_disable(IRQ_TIMER3);
    mix_drop();
    if (pcm_kind == KIND_RAMP && rest_ramp) {
        /* The ramp has reached the midpoint: that is the rest level now. */
        rest_ramp = 0;
        rest_mid = 1;
    }
    if (backend == AUDIO_BACKEND_PWM) {
        pwm_silence();
    } else {
        audio_trace_pcm_end(pcm.n, pcm.last, pcm_t0_ms, timer_now_us() - pcm_t0_us);
    }
    playing = seq.active;
}

/*
 * IRQ context. The ramp follows the clock as far as the step rule allows:
 * each write is at most the ramp's largest step, ramp_step, above the last
 * one. It plays at full scale on range 256, so a byte step is a duty step. The ramp rises, so the furthest index the rule allows is a binary
 * search, a few turns. Every write is at least the next index, which the
 * rule always allows. So a late IRQ skips steps only while the jump stays
 * that small, and otherwise the ramp takes longer. The next step is due at
 * its grid time, or one sample period after now when that time has
 * passed. The last index is duty 128. The IRQ after it ends the play,
 * and pcm_finish() sets the midpoint rest level with no jump.
 */
static void ramp_run(uint32_t now)
{
    uint32_t want = (uint32_t)(((uint64_t)(now - pcm.origin_us) * pcm.mul) >> 32);
    uint32_t i = ramp_i;
    uint32_t next;

    if (ramp_i >= pcm.len) {
        /* One sample period after the last index, as a plain play ends. */
        pcm.active = 0;
        pcm_finish();
        return;
    }
    if (want > pcm.len - 1) {
        want = pcm.len - 1;
    }
    if (i > 0 && want > i) {
        uint32_t lim = ramp[i - 1] + ramp_step;
        uint32_t hi = want;

        /* Largest index in [i, hi] whose step from the last write is allowed. */
        while (i < hi) {
            uint32_t mid = i + (hi - i + 1) / 2;

            if (ramp[mid] <= lim) {
                i = mid;
            } else {
                hi = mid - 1;
            }
        }
    }
    pcm.last = i;
    pcm.last_n = i;
    pcm.n++;
    pcm_out(i);
    ramp_i = i + 1;
    pcm.next_q = ramp_i;
    next = audio_pcm_next_us(&pcm);
    if ((int32_t)(next - now) <= 0) {
        next = now + RAMP_STEP_US;
    }
    timer_oneshot_arm(PCM_CHANNEL, next);
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

    if (pcm_kind == KIND_RAMP) {
        ramp_run(now);
        return;
    }
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
            pcm_out(pos);
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
    uint32_t g = gain_target;

    pcm = pcm_req;
    pcm_kind = pcm_req_kind;
    voice = voice_req;
    ramp_i = 0;
    /* The play starts at the target: nothing fades in. */
    gain_cur = g;
    gain_step = audio_gain_step(pcm.rate);
    gain_n = pcm.last_n;
    if (backend == AUDIO_BACKEND_PWM) {
        if (pcm_kind == KIND_RAMP) {
            /* The ramp starts from its first sample, not from the midpoint. */
            audio_pwm_pcm_mode_at(audio_pcm_duty(pcm.data[0], AUDIO_PCM_PWM_RANGE));
        } else {
            audio_pwm_pcm_mode();
        }
    } else if (pcm_kind == KIND_VOICE) {
        audio_trace_voice(pcm.rate, (uint32_t)voice.count, pcm.len, g, pcm_req_crc, clock_ms);
    } else {
        audio_trace_pcm(pcm.rate, pcm.len, pcm_kind == KIND_RAMP ? AUDIO_GAIN_FULL : g, pcm_req_crc,
                        clock_ms, (pcm.flags & AUDIO_LOOP) != 0);
    }
    pcm_t0_ms = clock_ms;
    pcm_t0_us = timer_now_us();
    /* The engine works on the low word and rebases, so its times wrap safely. */
    pcm.origin_us = (uint32_t)pcm_t0_us;
    mix_drop();
    if (mix_req_set) {
        /* The overlay waited with the play: both start on grid sample 0. */
        mix = mix_req;
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
    /* A ramp that waits or plays ends early: the rest level stays duty 0. */
    rest_ramp = 0;
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

/*
 * Tick context. Starts a tone play: sample 0 sounds now, on its own grid
 * at ROBCO_AUDIO_TONE_RATE. The template holds no divide to do here. It
 * prints no trace line: output() prints the tone's own.
 */
static void tone_begin(void)
{
    pcm = tone_pcm;
    pcm_kind = KIND_TONE;
    gain_cur = gain_target;
    gain_step = TONE_GAIN_STEP;
    gain_n = pcm.last_n;
    pcm_t0_ms = clock_ms;
    pcm_t0_us = timer_now_us();
    pcm.origin_us = (uint32_t)pcm_t0_us;
    mix_drop();
    if (backend == AUDIO_BACKEND_PWM) {
        audio_pwm_pcm_mode();
    }
    timer_compare_clear(PCM_CHANNEL);
    irq_enable(IRQ_TIMER3);
    pcm_run(pcm.origin_us);
}

/*
 * Tick context, rest at the midpoint only. A new note, or a rest, of the
 * sequence: the next sample starts it at an edge going up. The sample IRQ
 * takes the height from the gain. The first sounding note starts the play.
 * A note after a sounding note, `was` not 0, keeps the gain and its move.
 * A note after a rest starts at the target, as a play does: the rest
 * wrote 128, so the height has nothing to jump from.
 */
static void tone_note(uint32_t hz, uint32_t was)
{
    audio_square_start(&square, hz, ROBCO_AUDIO_TONE_RATE,
                       audio_square_amp_g(ROBCO_AUDIO_TONE_PEAK, gain_target));
    if (hz && !(pcm.active && pcm_kind == KIND_TONE)) {
        tone_begin();
    } else if (hz && !was) {
        gain_cur = gain_target;
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
            audio_trace_out(0, gain_target, tone_t(clock_ms));
            /* Traced already: output() prints only a new tone. PWM keeps its writes. */
            sounding = 0;
        }
        tone_mark_set = 0;
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
    if (pcm.active && pcm_kind == KIND_TONE && !seq.active) {
        /* The sequence has ended: the pin goes back to 128 from the last sample. */
        pcm_halt();
    }
    if (!seq.active && !pcm.active) {
        tone_mark_set = 0;
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

/*
 * Something plays: true from the moment a sound, a stop or the rest ramp
 * is accepted until the pin is back at rest. That covers a request that
 * waits for its tick, a tone, a sequence or a PCM play that sounds, and a
 * stop or cut not yet applied. Every busy rule starts here. IRQs masked.
 */
static int busy_locked(void)
{
    return req.valid || playing;
}

/* A stop waits for its tick and nothing was accepted after it. IRQs masked. */
static int stop_pending_locked(void)
{
    return req.valid && req.stop;
}

/*
 * A new sound without AUDIO_PRIORITY is refused while something plays. It
 * may take the place of a pending stop: the tick still ends the old sound
 * with its stop line first. IRQs masked.
 */
static int start_refused_locked(void)
{
    return busy_locked() && !stop_pending_locked();
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
    if (!replace && start_refused_locked()) {
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

/*
 * A change of target while a play sounds prints one op=gain line. With
 * nothing playing, in a rest of a sequence, or with a start that waits
 * for its tick, the next start line carries the gain instead. In a sample
 * play, a voice or a centred tone the time is that of op=pcm_mix: the
 * play's next grid sample that has not passed, where the sample IRQ first
 * slews. A centred tone reads that time on the audio clock, the clock of
 * its own lines, so the line and the tick lines share one base. A pulse
 * tone and the rest ramp take the audio clock of the last tick.
 */
void audio_set_gain(uint32_t gain)
{
    uint32_t s;

    if (gain > AUDIO_GAIN_FULL) {
        gain = AUDIO_GAIN_FULL;
    }
    s = irq_save();
    if (gain != gain_target) {
        uint64_t now = timer_now_us();

        if (backend >= 0 && pcm.active && pcm_kind != KIND_RAMP) {
            /*
             * The change starts after the grid sample due now: the one
             * before the join sample, where the op=gain line puts it.
             * A change under way first takes the steps it is owed.
             */
            audio_gain_follow(&gain_cur, &gain_n, gain_target, gain_step,
                              audio_pcm_join(&pcm, (uint32_t)now) - 1u);
        }
        gain_target = gain;
        if (backend >= 0 && backend != AUDIO_BACKEND_PWM && playing && !req.valid) {
            if (pcm.active && (pcm_kind == KIND_SAMPLE || pcm_kind == KIND_VOICE)) {
                audio_trace_gain(gain, pcm_t0_ms,
                                 (now - pcm_t0_us) +
                                 (uint32_t)(audio_pcm_join_us(&pcm, (uint32_t)now) -
                                            (uint32_t)now));
            } else if (seq.active && !sounding) {
                /* A rest: the next note's line carries the gain. */
            } else if (pcm.active && pcm_kind == KIND_TONE) {
                /*
                 * The audio clock at the join sample, as the next tick
                 * would count it from the last: clock_ms and acc_us
                 * hold the time of last_us. The join lies after the
                 * last tick, so the time never goes back.
                 */
                uint32_t join = audio_pcm_join_us(&pcm, (uint32_t)now);

                tone_mark_ms = clock_ms + (acc_us + (join - last_us)) / 1000u;
                tone_mark_set = 1;
                audio_trace_gain(gain, tone_mark_ms, 0);
            } else {
                tone_mark_ms = clock_ms;
                tone_mark_set = 1;
                audio_trace_gain(gain, clock_ms, 0);
            }
        }
    }
    irq_restore(s);
}

void audio_set_volume(unsigned percent)
{
    audio_set_gain((percent > 100 ? 100 : percent) * 100u);
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
        ret = start_refused_locked();
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
    if (!(flags & AUDIO_PRIORITY) && start_refused_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    pcm_cut_locked();
    pcm_req = p;
    pcm_req_crc = crc;
    pcm_req_kind = KIND_SAMPLE;
    req.stop = 0;
    req.pcm = 1;
    req.notes = 0;
    req.valid = 1;
    irq_restore(s);
    return 0;
}

/*
 * A voice is a play of len samples, the sum of every span's n, on the
 * PCM engine with no buffer. The engine gives the position, and pcm_out()
 * computes the sample there. The span check and the CRC run here, in the
 * main context, never in an IRQ.
 */
int audio_play_voice(const audio_voice_span_t *spans, size_t count, uint32_t rate_hz,
                     unsigned flags)
{
    audio_pcm_t p;
    audio_voice_t v;
    uint32_t len = 0;
    uint32_t crc = 0;
    uint32_t s;
    int ret;

    if (backend < 0) {
        return AUDIO_ERR_NO_INIT;
    }
    ret = audio_voice_check(spans, count, rate_hz, flags, &len);
    if (ret) {
        return ret;
    }
    if (!pcm_irq) {
        return AUDIO_ERR_NO_IRQ;
    }
    if (!(flags & AUDIO_PRIORITY)) {
        s = irq_save();
        ret = start_refused_locked();
        irq_restore(s);
        if (ret) {
            return AUDIO_ERR_BUSY;
        }
    }
    audio_pcm_start(&p, 0, len, rate_hz, 0, 0);
    audio_voice_start(&v, spans, count);
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(spans, count * sizeof(*spans));
    }
    s = irq_save();
    if (!(flags & AUDIO_PRIORITY) && start_refused_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    pcm_cut_locked();
    pcm_req = p;
    pcm_req_crc = crc;
    pcm_req_kind = KIND_VOICE;
    voice_req = v;
    req.stop = 0;
    req.pcm = 1;
    req.notes = 0;
    req.valid = 1;
    irq_restore(s);
    return 0;
}

/*
 * The play a queued buffer would join: the pending start, or the one that
 * plays. Only a sample play takes one: the ramp and a voice read no
 * buffer to chain behind. IRQs masked.
 */
static audio_pcm_t *queue_target_locked(void)
{
    if (req.valid) {
        return req.pcm && !req.stop && pcm_req_kind == KIND_SAMPLE ? &pcm_req : 0;
    }
    return pcm.active && pcm_kind == KIND_SAMPLE ? &pcm : 0;
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
        audio_pcm_t *t = queue_target_locked();

        if (t == &pcm) {
            /* An end already passed, the IRQ still due: start at the join sample. */
            audio_pcm_chain_at(t, data, len, timer_now_lo());
        } else {
            audio_pcm_chain(t, data, len);
        }
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
 * The play an overlay would join: a sample play or a voice, not the ramp
 * or a tone. A tone never waits in pcm_req: the tick starts it.
 * Sets *pending when that play still waits for its tick. IRQs masked.
 */
static audio_pcm_t *mix_target_locked(int *pending)
{
    if (req.valid) {
        *pending = 1;
        return req.pcm && !req.stop && pcm_req_kind != KIND_RAMP ? &pcm_req : 0;
    }
    *pending = 0;
    return pcm.active && (pcm_kind == KIND_SAMPLE || pcm_kind == KIND_VOICE) ? &pcm : 0;
}

/*
 * 0 when an overlay can start at `now`, else AUDIO_ERR_STATE or
 * AUDIO_ERR_BUSY. A play whose last sample has passed, its end IRQ still
 * due, counts as no play: the overlay's first byte would never sound.
 * IRQs masked.
 */
static int mix_check_locked(uint32_t now)
{
    int pending;
    audio_pcm_t *t = mix_target_locked(&pending);

    if (!t || (!pending && !audio_pcm_join_plays(t, now))) {
        return AUDIO_ERR_STATE;
    }
    return (pending ? mix_req_set : mix.active) ? AUDIO_ERR_BUSY : 0;
}

int audio_pcm_mix(const uint8_t *data, size_t len)
{
    mix_t m = { data, (uint32_t)len, 0, 1 };
    uint64_t now;
    uint32_t crc = 0;
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
    ret = mix_check_locked(timer_now_lo());
    irq_restore(s);
    if (ret) {
        return ret;
    }
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(data, len);
    }
    s = irq_save();
    now = timer_now_us();
    ret = mix_check_locked((uint32_t)now);
    if (ret) {
        irq_restore(s);
        return ret;
    }
    mix_target_locked(&pending);
    if (pending) {
        mix_req = m;
        mix_req_crc = crc;
        mix_req_set = 1;
    } else {

        /* From the play's next grid sample that has not passed. */
        m.at = audio_pcm_join(&pcm, (uint32_t)now);
        mix = m;
        if (backend != AUDIO_BACKEND_PWM) {
            /*
             * The trace time is derived from the play's grid, never stored.
             * 64 bits: a looped play can run past the 32-bit wrap. The join
             * lies after the read, never before, and within one sample.
             */
            audio_trace_pcm_mix(m.len, crc, pcm_t0_ms,
                                (now - pcm_t0_us) +
                                (uint32_t)(audio_pcm_join_us(&pcm, (uint32_t)now) - (uint32_t)now));
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
        if (i && (uint32_t)(ramp[i] - ramp[i - 1]) > ramp_step) {
            ramp_step = (uint32_t)(ramp[i] - ramp[i - 1]);
        }
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
    if (rest_mid || rest_ramp) {
        return 0;
    }
    if (!pcm_irq) {
        return AUDIO_ERR_NO_IRQ;
    }
    /* A pending stop counts: the ramp starts from rest, after the stop. */
    s = irq_save();
    if (busy_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    irq_restore(s);
    ramp_fill();
    audio_pcm_start(&p, ramp, RAMP_LEN, RAMP_RATE, 0, 0);
    /*
     * The tone play's divides run here, in the main context. It loops so
     * it never ends by itself: the tick halts it. The square follows
     * last_n, which does not wrap with the loop.
     */
    audio_pcm_start(&tone_pcm, 0, AUDIO_PCM_LEN_MAX, ROBCO_AUDIO_TONE_RATE, AUDIO_LOOP, 0);
    if (backend == AUDIO_BACKEND_TRACE) {
        crc = crc32(ramp, RAMP_LEN);
    }
    s = irq_save();
    if (rest_mid || rest_ramp) {
        irq_restore(s);
        return 0;
    }
    if (busy_locked()) {
        irq_restore(s);
        return AUDIO_ERR_BUSY;
    }
    pcm_req = p;
    pcm_req_crc = crc;
    pcm_req_kind = KIND_RAMP;
    req.stop = 0;
    req.pcm = 1;
    req.notes = 0;
    req.valid = 1;
    rest_ramp = 1;
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
    if (!(flags & AUDIO_PRIORITY) && start_refused_locked()) {
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
