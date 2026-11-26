#ifndef ROBCO_AUDIO_H
#define ROBCO_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Audio: square-wave tones, note sequences, short tunes and 8-bit PCM.
 * Tones run from a tick callback. PCM runs from system timer compare
 * channel 3, one IRQ per sample. Two backends: "pwm" drives the PWM block,
 * "trace" prints one marker per change for tests and for boards without PWM.
 */

/* Error codes. */
#define AUDIO_ERR_NO_INIT   (-1)    /* audio_init() has not succeeded */
#define AUDIO_ERR_NO_IRQ    (-2)    /* irq_init() has not run */
#define AUDIO_ERR_NO_TICK   (-3)    /* the tick does not run, or CPU IRQs are masked */
#define AUDIO_ERR_TICK_FULL (-4)    /* no free tick callback slot */
#define AUDIO_ERR_NO_PWM    (-5)    /* PWM forced but its registers abort; trace runs instead */
#define AUDIO_ERR_RANGE     (-6)    /* frequency out of range */
#define AUDIO_ERR_ARG       (-7)    /* bad argument */
#define AUDIO_ERR_BUSY      (-8)    /* playing, and AUDIO_PRIORITY not set */
#define AUDIO_ERR_PARSE     (-9)    /* malformed tune text */
#define AUDIO_ERR_FULL      (-10)   /* tune has more notes than the buffer */
#define AUDIO_ERR_STATE     (-11)   /* call not allowed after audio_init() */
#define AUDIO_ERR_PIN       (-12)   /* a PWM pin did not take its function; trace runs instead */
#define AUDIO_ERR_TIMER     (-13)   /* the system timer does not advance */

/* Audible range. A step with hz 0 is a rest. */
#define AUDIO_HZ_MIN        20u
#define AUDIO_HZ_MAX        20000u

/* PCM sample rates accepted by audio_play_pcm(). */
#define AUDIO_PCM_RATE_MIN  4000u
#define AUDIO_PCM_RATE_MAX  22050u

/* Longest PCM buffer, in bytes. */
#define AUDIO_PCM_LEN_MAX   0x7fffffffu

/* PWM range in PCM mode: 9.6 MHz / 256 gives a 37.5 kHz carrier. */
#define AUDIO_PCM_PWM_RANGE 256u

/* audio_play() flags. */
#define AUDIO_LOOP          1u      /* restart at the first step after the last */
#define AUDIO_PRIORITY      2u      /* replace what plays now instead of failing */

/* Backend choices for ROBCO_AUDIO_BACKEND and audio_select(). */
#define AUDIO_BACKEND_AUTO  0
#define AUDIO_BACKEND_PWM   1
#define AUDIO_BACKEND_TRACE 2

/* Probe results, see audio_probe_result(). */
#define AUDIO_PROBE_SKIPPED 0       /* trace forced, PWM not probed */
#define AUDIO_PROBE_OK      1       /* registers respond and keep writes */
#define AUDIO_PROBE_ABORT   2       /* a read took a data abort */
#define AUDIO_PROBE_IGNORED 3       /* reads work but a write did not stick */

/* No pin for this PWM channel. */
#define AUDIO_PIN_NONE      (-1)

/* Clock manager sources for ROBCO_AUDIO_CLOCK_SRC. */
#define AUDIO_CLK_SRC_OSC   1u      /* 19.2 MHz oscillator */
#define AUDIO_CLK_SRC_PLLD  6u      /* 500 MHz PLLD */

/* PWM clock the divisor aims for, before the per-tone range. */
#define AUDIO_PWM_CLOCK_TARGET  9600000u
#define AUDIO_PWM_DIVI_MIN      2u
#define AUDIO_PWM_DIVI_MAX      4095u

/* One step: a tone of `hz` (0 for a rest) for `ms` milliseconds. */
typedef struct {
    uint32_t hz;
    uint32_t ms;
} audio_note_t;

/* Front end, src/drv/audio.c. */

/* Forces a backend before audio_init(). Overrides ROBCO_AUDIO_BACKEND. */
int audio_select(int backend);

/*
 * Sets the output pins before audio_init(). pin0 carries PWM channel 1,
 * pin1 channel 2. AUDIO_PIN_NONE turns a channel off. fn is a GPIO_FN_*
 * code. Overrides the ROBCO_AUDIO_PIN* macros and the per-board default.
 */
int audio_set_pins(int pin0, unsigned fn0, int pin1, unsigned fn1);

/* Reads the pins in use, or the ones audio_init() will use. */
void audio_get_pins(int *pin0, unsigned *fn0, int *pin1, unsigned *fn1);

/*
 * Picks a backend, registers the tick callback and checks the tick runs.
 * Needs irq_init(), tick_init() and irq_cpu_enable() first. Returns 0 or
 * an AUDIO_ERR_* code. When PWM is forced and its registers abort, it
 * returns AUDIO_ERR_NO_PWM. When a pin cannot be switched to its PWM
 * function, it returns AUDIO_ERR_PIN. Both still install the trace backend.
 * A forced PWM whose registers ignore writes is used anyway and returns 0.
 *
 * Needs a running system timer. When the timer does not advance it
 * returns AUDIO_ERR_TIMER after a million reads that see no change,
 * instead of waiting forever. Tones and PCM need the timer too: without
 * it the tick and the PCM compare never fire, so nothing plays.
 */
int audio_init(void);

/* "pwm", "trace", or "none" before audio_init(). */
const char *audio_backend(void);

/* AUDIO_PROBE_* result of the last probe. */
int audio_probe_result(void);

/* "ok", "abort", "ignored" or "skipped". */
const char *audio_probe_name(int result);

/* Plays one tone, non-blocking. Replaces what plays now. hz 0 is a rest. */
int audio_tone(uint32_t hz, uint32_t ms);

/*
 * Stops. PCM output stops before the call returns. Tones stop at the next
 * tick, which also prints the trace stop line.
 */
void audio_stop(void);

/*
 * Non-zero while a tone, rest, sequence or PCM buffer plays, or a request
 * waits for the tick. After audio_stop() it turns 0 at the next tick.
 */
int audio_busy(void);

/* Duty-cycle volume, 0 to 100. Applies from the next note. */
void audio_set_volume(unsigned percent);

/*
 * Plays `count` steps. The array must stay valid until playback ends.
 * flags: AUDIO_LOOP, AUDIO_PRIORITY. Returns 0 or an AUDIO_ERR_* code.
 */
int audio_play(const audio_note_t *seq, size_t count, unsigned flags);

/*
 * Plays 8-bit unsigned mono PCM in the background. The library does not
 * copy: `data` must stay valid until playback ends, or until audio_stop()
 * or a call that replaces the sound returns. Such a call stops reading the
 * buffer before it returns. The new sound starts at the next tick.
 * flags: AUDIO_LOOP, AUDIO_PRIORITY, as for audio_play().
 * Returns 0 or AUDIO_ERR_* (NO_INIT, ARG for NULL, len 0, len above
 * AUDIO_PCM_LEN_MAX or unknown flags, RANGE for the rate, BUSY when
 * something plays and AUDIO_PRIORITY is not set, NO_IRQ when another
 * handler holds the timer channel 3 IRQ).
 */
int audio_play_pcm(const uint8_t *data, size_t len, uint32_t rate_hz, unsigned flags);

/*
 * Queues the next PCM buffer behind the one that plays. It starts with no
 * gap when the current buffer ends, at the same rate. One buffer can wait.
 * The library does not copy: `data` must stay valid until that buffer has
 * finished playing, or until audio_stop() or a call that replaces the sound
 * returns.
 * Returns 0, or AUDIO_ERR_NO_INIT, AUDIO_ERR_ARG (NULL, len 0, len above
 * AUDIO_PCM_LEN_MAX), AUDIO_ERR_STATE when no PCM is playing or pending
 * (start with audio_play_pcm()), AUDIO_ERR_BUSY when a buffer already waits,
 * AUDIO_ERR_STATE when the playing PCM loops (AUDIO_LOOP never ends).
 */
int audio_pcm_queue(const uint8_t *data, size_t len);

/*
 * 1 when audio_pcm_queue() would accept a buffer now: PCM plays and no
 * buffer waits. When this turns 1 again after a queue call, the buffer
 * that was playing at the time of that call has finished and is free.
 */
int audio_pcm_can_queue(void);

/*
 * Adds one overlay buffer to the PCM that plays, from its next sample, at
 * its rate: each output sample is play + data - 128, clamped to 0 to 255,
 * then scaled by the volume. Like audio_pcm_queue(), it also takes a play
 * that still waits for its tick; both then start on the same sample. The
 * end or a cut of the play drops the overlay, so an overlay longer than
 * what is left of the play stops with it. The library does not copy:
 * `data` must stay valid until the overlay ends or is dropped.
 * Returns 0, or AUDIO_ERR_NO_INIT, AUDIO_ERR_ARG (NULL, len 0, len above
 * AUDIO_PCM_LEN_MAX), AUDIO_ERR_STATE when no PCM plays or waits to start,
 * AUDIO_ERR_BUSY when an overlay already runs or waits.
 */
int audio_pcm_mix(const uint8_t *data, size_t len);

/* 1 while an overlay plays or waits for its play to start. */
int audio_pcm_mix_busy(void);

/*
 * Moves the rest level to the midpoint. The first call plays a ramp from
 * duty 0 to duty 128 at range AUDIO_PCM_PWM_RANGE: 882 samples at 22050 Hz
 * on the curve 3t^2 - 2t^3, at full scale whatever the volume. From then
 * on every silence the driver writes is that range at duty 128: the end of
 * a play, a cut, a stop, the end of a tone. Later calls do nothing and
 * return 0. Until the first call, the rest level stays duty 0.
 * Returns 0, or AUDIO_ERR_NO_INIT, AUDIO_ERR_NO_IRQ (another handler holds
 * the timer channel 3 IRQ), AUDIO_ERR_BUSY when something plays or waits:
 * the rest level is then unchanged and a later call can try again.
 */
int audio_rest_mid(void);

/*
 * Samples output since audio_init(). If `play` is not NULL, it gets the
 * samples output by the current or last PCM play, counted from
 * audio_play_pcm() over every queued buffer that followed. For tests.
 */
uint32_t audio_pcm_samples(uint32_t *play);

/* Parses an RTTTL tune into an internal buffer and plays it. */
int audio_play_rtttl(const char *text, unsigned flags);

/* The standard short beep. Replaces what plays now. */
int audio_beep(void);

/* Prints queued trace records. Main context only. */
void audio_poll(void);

/* Polls and sleeps with wfi until idle. Returns at once if CPU IRQs are masked. */
void audio_wait(void);

/* Pure helpers, src/lib/. */

/* Sequencer state. Treat the fields as private. */
typedef struct {
    const audio_note_t *notes;
    size_t count;
    size_t pos;             /* current step */
    unsigned flags;
    uint32_t start_ms;      /* when the current step began */
    uint32_t dur_ms;        /* length of the current step */
    uint32_t period_ms;     /* sum of all steps, saturated at UINT32_MAX */
    uint32_t hz;            /* what sounds now, 0 for silence */
    uint32_t step;          /* counts every step change */
    int active;
} audio_seq_t;

/* Starts `notes` at `now_ms`. Returns 1 if something plays, 0 if empty. */
int audio_seq_start(audio_seq_t *s, const audio_note_t *notes, size_t count,
                    unsigned flags, uint32_t now_ms);

/* Stops. hz becomes 0. */
void audio_seq_stop(audio_seq_t *s);

/*
 * Moves to the next due step change at or before `now_ms`. Returns 1 and
 * stores in *at_ms when the change was due, or 0 when nothing is due.
 * Call it again until it returns 0. Wraps at 2^32 ms.
 */
int audio_seq_poll(audio_seq_t *s, uint32_t now_ms, uint32_t *at_ms);

/* Frequency in Hz, rounded, of semitone 0 (C) to 11 (B) in octave 0 to 8. 0 if out of range. */
uint32_t audio_note_hz(unsigned semitone, unsigned octave);

/*
 * Parses RTTTL `text` into `out`. Returns the step count or a negative
 * AUDIO_ERR_*. On error, *err_pos (if not NULL) gets the byte offset.
 */
int audio_rtttl_parse(const char *text, audio_note_t *out, size_t max, size_t *err_pos);

/* PWM settings for one tone. */
typedef struct {
    uint32_t divi;          /* CM_PWMDIV integer part, bits 23:12 */
    uint32_t clock_hz;      /* src_hz / divi */
    uint32_t rng;           /* RNG register */
    uint32_t dat;           /* DAT register */
} audio_pwm_cfg_t;

/*
 * Computes the divisor, range and data for `hz` at `vol` percent from a
 * clock source of `src_hz`. Returns 0, AUDIO_ERR_ARG or AUDIO_ERR_RANGE.
 */
int audio_pwm_calc(uint32_t src_hz, uint32_t hz, unsigned vol, audio_pwm_cfg_t *out);

/* PCM playback state. Treat the fields as private. */
typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t rate;
    unsigned flags;
    uint32_t mul;           /* floor(rate * 2^32 / 1000000) */
    uint32_t inv;           /* ceil(1000000 * 2^24 / rate) */
    uint32_t unit_us;       /* 1000000 / gcd(rate, 1000000) */
    uint32_t unit_n;        /* samples in unit_us */
    uint32_t origin_us;     /* time of sample origin_pos */
    uint32_t origin_pos;    /* sample index at origin_us, signed after a switch */
    uint32_t next_q;        /* samples after the origin of the next sample due */
    uint32_t last;          /* last index output, in the current buffer */
    uint32_t n;             /* samples output, over every chained buffer */
    const uint8_t *next;    /* buffer that waits, or NULL */
    uint32_t next_len;
    uint32_t switches;      /* times a waiting buffer took over */
    int active;
} audio_pcm_t;

/* audio_pcm_poll() results. */
#define AUDIO_PCM_NONE      0       /* nothing new is due */
#define AUDIO_PCM_SAMPLE    1       /* output the sample at *pos */
#define AUDIO_PCM_END       2       /* the last sample has played */

/* Checks audio_play_pcm() arguments. Returns 0, AUDIO_ERR_ARG or AUDIO_ERR_RANGE. */
int audio_pcm_check(const uint8_t *data, size_t len, uint32_t rate_hz, unsigned flags);

/* Starts playback with sample 0 at `now_us`. Arguments must pass audio_pcm_check(). */
void audio_pcm_start(audio_pcm_t *p, const uint8_t *data, size_t len, uint32_t rate_hz,
                     unsigned flags, uint32_t now_us);

/*
 * Position from the clock: sample floor(elapsed_us * rate / 1000000).
 * Returns AUDIO_PCM_SAMPLE with *pos when a sample not yet output is due,
 * AUDIO_PCM_NONE before that, AUDIO_PCM_END once a non-looped buffer has
 * played to its end. Samples between two calls are skipped. Wraps at 2^32 us.
 */
int audio_pcm_poll(audio_pcm_t *p, uint32_t now_us, uint32_t *pos);

/*
 * Chains `data` behind the current buffer: when the position passes the
 * end, it continues at pos - len in `data`, on the same clock grid. One
 * buffer can wait. Returns 0, or AUDIO_ERR_STATE when the play is not
 * active or loops, AUDIO_ERR_BUSY when a buffer already waits. Arguments
 * must pass audio_pcm_check().
 */
int audio_pcm_chain(audio_pcm_t *p, const uint8_t *data, size_t len);

/* Time in us, wrapping, when the next sample is due. */
uint32_t audio_pcm_next_us(const audio_pcm_t *p);

/*
 * Volume around the midpoint: 128 + (s - 128) * vol / 100, rounded to the
 * nearest step, a half step away from 128. vol is capped at 100.
 */
uint8_t audio_pcm_scale(uint8_t s, unsigned vol);

/* PWM data value for sample `s` at range `rng`: s * rng / 256. */
uint32_t audio_pcm_duty(uint8_t s, uint32_t rng);

#endif
