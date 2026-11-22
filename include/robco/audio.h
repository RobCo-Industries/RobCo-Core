#ifndef ROBCO_AUDIO_H
#define ROBCO_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Square-wave audio: tones, note sequences and short tunes. Playback runs
 * from a tick callback. Two backends: "pwm" drives the PWM block, "trace"
 * prints one marker per change for tests and for boards without PWM.
 */

/* Error codes. */
#define AUDIO_ERR_NO_INIT   (-1)    /* audio_init() has not succeeded */
#define AUDIO_ERR_NO_IRQ    (-2)    /* irq_init() has not run */
#define AUDIO_ERR_NO_TICK   (-3)    /* the tick does not run, or CPU IRQs are masked */
#define AUDIO_ERR_TICK_FULL (-4)    /* no free tick callback slot */
#define AUDIO_ERR_NO_PWM    (-5)    /* PWM forced but the registers abort or ignore writes */
#define AUDIO_ERR_RANGE     (-6)    /* frequency out of range */
#define AUDIO_ERR_ARG       (-7)    /* bad argument */
#define AUDIO_ERR_BUSY      (-8)    /* playing, and AUDIO_PRIORITY not set */
#define AUDIO_ERR_PARSE     (-9)    /* malformed tune text */
#define AUDIO_ERR_FULL      (-10)   /* tune has more notes than the buffer */
#define AUDIO_ERR_STATE     (-11)   /* call not allowed after audio_init() */

/* Audible range. A step with hz 0 is a rest. */
#define AUDIO_HZ_MIN        20u
#define AUDIO_HZ_MAX        20000u

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
 * an AUDIO_ERR_* code. When PWM is forced but fails the probe, it returns
 * AUDIO_ERR_NO_PWM and still installs the trace backend.
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

/* Stops at the next tick. */
void audio_stop(void);

/* Non-zero while a tone, rest or sequence plays or waits to start. */
int audio_busy(void);

/* Duty-cycle volume, 0 to 100. Applies from the next note. */
void audio_set_volume(unsigned percent);

/*
 * Plays `count` steps. The array must stay valid until playback ends.
 * flags: AUDIO_LOOP, AUDIO_PRIORITY. Returns 0 or an AUDIO_ERR_* code.
 */
int audio_play(const audio_note_t *seq, size_t count, unsigned flags);

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

#endif
