## Audio (`audio.h`)

`audio.h` plays square-wave tones, note sequences and short tunes: a power-on beep, key clicks, simple jingles. It has two backends. `pwm` drives the PWM block. `trace` prints one marker per change, for tests and for boards where PWM does not work. Playback runs from a tick callback, so every call returns at once. Nothing is allocated.

Init order: after step 10 of the [initialisation order](../API.md#initialisation-order). Call `audio_init()` once `irq_init()`, `tick_init(hz)` and `irq_cpu_enable()` have run. It takes one of the four tick callback slots.

| Constant | Value | Meaning |
|---|---|---|
| `AUDIO_ERR_NO_INIT` | `-1` | `audio_init()` has not succeeded |
| `AUDIO_ERR_NO_IRQ` | `-2` | `irq_init()` has not run |
| `AUDIO_ERR_NO_TICK` | `-3` | The tick does not run, or CPU IRQs are masked |
| `AUDIO_ERR_TICK_FULL` | `-4` | No free tick callback slot |
| `AUDIO_ERR_NO_PWM` | `-5` | PWM was forced, but its registers abort |
| `AUDIO_ERR_RANGE` | `-6` | A frequency is outside `AUDIO_HZ_MIN` to `AUDIO_HZ_MAX` |
| `AUDIO_ERR_ARG` | `-7` | Bad argument |
| `AUDIO_ERR_BUSY` | `-8` | Something plays and `AUDIO_PRIORITY` is not set |
| `AUDIO_ERR_PARSE` | `-9` | Malformed tune text |
| `AUDIO_ERR_FULL` | `-10` | The tune has more notes than the buffer |
| `AUDIO_ERR_STATE` | `-11` | The call is only allowed before `audio_init()` |
| `AUDIO_HZ_MIN`, `AUDIO_HZ_MAX` | `20u`, `20000u` | Accepted tone range. `0` is a rest. |
| `AUDIO_LOOP` | `1u` | Restart at the first step after the last |
| `AUDIO_PRIORITY` | `2u` | Replace what plays now |
| `AUDIO_BACKEND_AUTO`, `_PWM`, `_TRACE` | `0`, `1`, `2` | Backend choice |
| `AUDIO_PROBE_SKIPPED`, `_OK`, `_ABORT`, `_IGNORED` | `0` to `3` | Probe result |
| `AUDIO_PIN_NONE` | `-1` | Channel not used |

```c
typedef struct {
    uint32_t hz;    /* 0 for a rest */
    uint32_t ms;
} audio_note_t;
```

### Configuration

Set these in `robco_config.h`. All are optional.

| Macro | Default | Meaning |
|---|---|---|
| `ROBCO_AUDIO_BACKEND` | `AUDIO_BACKEND_AUTO` | `1` forces PWM, `2` forces trace |
| `ROBCO_AUDIO_PIN0`, `ROBCO_AUDIO_FN0` | per board, see below | Pin and `GPIO_FN_*` code for PWM channel 1 |
| `ROBCO_AUDIO_PIN1`, `ROBCO_AUDIO_FN1` | per board | Pin and code for PWM channel 2. Only read when `ROBCO_AUDIO_PIN0` is set. Leave it out for mono. |
| `ROBCO_AUDIO_CLOCK_SRC` | `AUDIO_CLK_SRC_OSC` | PWM clock source: oscillator (19.2 MHz) or `AUDIO_CLK_SRC_PLLD` (500 MHz) |
| `ROBCO_AUDIO_VOLUME` | `100` | Start volume in percent |
| `ROBCO_AUDIO_BEEP_HZ`, `ROBCO_AUDIO_BEEP_MS` | `1000`, `80` | The standard beep |
| `ROBCO_AUDIO_TUNE_MAX` | `64` | Notes in the `audio_play_rtttl()` buffer. The call also uses that many notes of stack. |
| `ROBCO_AUDIO_TRACE_QUEUE` | `64` | Trace records held between two `audio_poll()` calls |

Default pins when `ROBCO_AUDIO_PIN0` is not set:

| Board | Channel 1 | Channel 2 |
|---|---|---|
| Pi Zero, Zero W, Zero 2 W (from the board revision) | GPIO 18, ALT5 | none |
| Other BCM2835 and BCM2836 boards (Pi 1, Pi 2) | GPIO 40, ALT0 | GPIO 45, ALT0 |
| BCM2837 (Pi 3) | GPIO 40, ALT0 | GPIO 41, ALT0 |

The Pi Zero has no audio jack. Its PWM comes out on the header: GPIO 18 at ALT5, or GPIO 12 and 13 at ALT0 for two channels. Add a filter or a piezo there, see [Wiring](#wiring).

### Functions

```c
int audio_select(int backend);
int audio_set_pins(int pin0, unsigned fn0, int pin1, unsigned fn1);
void audio_get_pins(int *pin0, unsigned *fn0, int *pin1, unsigned *fn1);
```

Runtime overrides. Call them before `audio_init()`: afterwards they return `AUDIO_ERR_STATE`. `audio_select()` overrides `ROBCO_AUDIO_BACKEND`. `audio_set_pins()` overrides the macros and the board default. `pin0` carries PWM channel 1, `pin1` channel 2, and `AUDIO_PIN_NONE` turns a channel off. The function code is not checked against the pin: pick an alternate function that routes PWM to that pin. `audio_get_pins()` reads the pins in use, or the ones `audio_init()` will use. Any pointer may be `NULL`.

```c
int audio_init(void);
```

Checks the order, picks a backend and registers the tick callback. Returns `0` or an error:

1. `AUDIO_ERR_NO_IRQ` before `irq_init()`.
2. `AUDIO_ERR_NO_TICK` if CPU IRQs are masked, or if `tick_count()` does not move within 1.1 s. At `TICK_HZ_MIN` the check itself can take a second.
3. `AUDIO_ERR_TICK_FULL` if all tick slots are taken.

A failed call changes nothing and can be retried. A call after success returns `0`.

Backend selection:

| Choice | Probe | Backend | Returns |
|---|---|---|---|
| Auto | `ok` | `pwm` | `0` |
| Auto | `abort` or `ignored` | `trace` | `0` |
| PWM | `ok` or `ignored` | `pwm` | `0` |
| PWM | `abort` | `trace` | `AUDIO_ERR_NO_PWM` |
| Trace | `skipped` | `trace` | `0` |

The probe reads the PWM CTL and RNG1 registers and the two clock manager registers with `mmio_probe()`. If any read aborts, the result is `abort` and nothing is written. Otherwise it starts the PWM clock, writes a pattern to RNG1, reads it back and restores the old value. A different value gives `ignored`. `AUDIO_ERR_NO_PWM` is the one error that still leaves audio usable: the trace backend is installed, so check `audio_backend()`.

| Board | QEMU result |
|---|---|
| `raspi0`, `raspi1ap` | `abort`: QEMU has no PWM and the read aborts |
| `raspi2b`, `raspi3b` | `ignored`: reads return 0 and writes are dropped |

```c
const char *audio_backend(void);
int audio_probe_result(void);
const char *audio_probe_name(int result);
```

`audio_backend()` returns `"pwm"`, `"trace"`, or `"none"` before `audio_init()`. `audio_probe_result()` returns the `AUDIO_PROBE_*` code. `audio_probe_name()` turns it into `"ok"`, `"abort"`, `"ignored"` or `"skipped"`.

```c
int audio_tone(uint32_t hz, uint32_t ms);
int audio_beep(void);
```

Start one tone and return. `hz` `0` is a rest of `ms`. `ms` `0` does nothing. Both replace whatever plays. `audio_beep()` plays `ROBCO_AUDIO_BEEP_HZ` for `ROBCO_AUDIO_BEEP_MS`. Return `0`, `AUDIO_ERR_RANGE` or `AUDIO_ERR_NO_INIT`.

```c
int audio_play(const audio_note_t *seq, size_t count, unsigned flags);
```

Plays `count` steps in order. Steps with `ms` `0` are skipped. With `AUDIO_LOOP` the sequence repeats until `audio_stop()` or a replacement. Without `AUDIO_PRIORITY` the call returns `AUDIO_ERR_BUSY` while something plays. With it, the new sequence replaces the old one at the next tick. The library keeps the pointer: `seq` must stay valid until playback ends. Every `hz` is checked first: `AUDIO_ERR_RANGE` changes nothing. `count` `0` returns `0` and plays nothing.

```c
int audio_play_rtttl(const char *text, unsigned flags);
```

Parses an RTTTL tune into an internal buffer of `ROBCO_AUDIO_TUNE_MAX` notes and plays it. Same flags as `audio_play()`. Returns `0`, `AUDIO_ERR_PARSE`, `AUDIO_ERR_RANGE`, `AUDIO_ERR_FULL`, `AUDIO_ERR_BUSY` or `AUDIO_ERR_NO_INIT`. An empty tune returns `0` and plays nothing. The text may be freed once the call returns.

```c
void audio_stop(void);
int audio_busy(void);
void audio_set_volume(unsigned percent);
```

`audio_stop()` silences at the next tick. `audio_busy()` is non-zero while a tone, rest or sequence plays, or a request waits for the tick. `audio_set_volume()` sets the PWM duty cycle from `0` (silent) to `100` (50%, the loudest square wave). It applies from the next note. Values above 100 count as 100. The trace backend records it in `vol=`.

```c
void audio_poll(void);
void audio_wait(void);
```

`audio_poll()` prints the queued trace records. Call it from the main loop. It does nothing on the PWM backend. `audio_wait()` polls and sleeps with `wfi` until `audio_busy()` is `0`, then polls once more. It returns at once if CPU IRQs are masked, because the tick could not end the sound. A looped sequence never ends: call `audio_stop()` first.

### Tune notation

RTTTL, the ringtone text format:

```
tune     = name ":" [ setting { "," setting } ] ":" [ note { "," note } ]
setting  = ( "d" | "o" | "b" ) "=" number
note     = [ duration ] ( letter [ "#" ] | "p" ) [ "." ] [ octave ] [ "." ]
```

| Part | Rule |
|---|---|
| `name` | Any text without `:`. Ignored. |
| `d` | Default duration: `1`, `2`, `4`, `8`, `16` or `32` (whole to 1/32). Default `4`. |
| `o` | Default octave, `0` to `8`. Default `6`. |
| `b` | Beats per minute, `1` to `900`. Default `63`. A whole note lasts `240000 / b` ms, rounded down. |
| `letter` | `c` to `g`, `a`, `b`, any case. `#` raises a semitone. `e#` and `b#` are errors. |
| `p` | A rest |
| `.` | Dotted: 1.5 times the length. One dot only, before or after the octave. |

Spaces, tabs and newlines are allowed between tokens. Frequencies come from equal temperament with A4 = 440 Hz, rounded to whole hertz. A note below 20 Hz (octave 0 below A) gives `AUDIO_ERR_RANGE`. Example: `"jingle:d=8,o=5,b=200:c,g,e,p,4c6,a,4g."`.

### Pure functions

These are in `src/lib/` and tested on the host.

```c
uint32_t audio_note_hz(unsigned semitone, unsigned octave);
```

Frequency of semitone `0` (C) to `11` (B) in octave `0` to `8`, rounded to whole hertz. Integer maths from a millihertz table. `0` when out of range.

```c
int audio_rtttl_parse(const char *text, audio_note_t *out, size_t max, size_t *err_pos);
```

Parses RTTTL into `out` and returns the note count, or `AUDIO_ERR_ARG`, `AUDIO_ERR_PARSE`, `AUDIO_ERR_RANGE` or `AUDIO_ERR_FULL`. On error `*err_pos` gets the byte offset of the problem, if `err_pos` is not `NULL`. With `out` `NULL` it only counts, up to `max`.

```c
int audio_seq_start(audio_seq_t *s, const audio_note_t *notes, size_t count,
                    unsigned flags, uint32_t now_ms);
void audio_seq_stop(audio_seq_t *s);
int audio_seq_poll(audio_seq_t *s, uint32_t now_ms, uint32_t *at_ms);
```

The sequencer the tick uses. `s->hz` is what should sound. `audio_seq_poll()` moves past one due step change, stores when it was due in `*at_ms` and returns `1`. Call it until it returns `0`. A looped sequence that falls a whole loop behind skips the full loops at once. Times wrap at 2^32 ms.

```c
int audio_pwm_calc(uint32_t src_hz, uint32_t hz, unsigned vol, audio_pwm_cfg_t *out);
```

Picks the clock divisor nearest to a 9.6 MHz PWM clock (clamped to `2` to `4095`), then `rng = clock / hz` rounded and `dat = rng / 2 * vol / 100`. Returns `0`, `AUDIO_ERR_ARG` or `AUDIO_ERR_RANGE`. From the oscillator, 440 Hz gives divisor `2`, range `21818` and data `10909`.

### Context rules

| Function | Context |
|---|---|
| `audio_select()`, `audio_set_pins()`, `audio_init()` | Main context, once |
| `audio_tone()`, `audio_beep()`, `audio_play()`, `audio_stop()`, `audio_busy()`, `audio_set_volume()` | Main or IRQ context. They mask IRQs for a few instructions. |
| `audio_play_rtttl()` | Main context: it parses with a buffer on the stack |
| `audio_poll()`, `audio_wait()` | Main context only. They print with `k_printf()`. |

The trace backend never prints from the tick. The UART driver and `k_printf()` have no lock, so a print from IRQ context could interleave with the main flow. The tick puts each record in a queue of `ROBCO_AUDIO_TRACE_QUEUE` entries, and `audio_poll()` or `audio_wait()` prints them. When the queue is full, records are dropped and the next poll prints `@RC stage=<name> ev=audio_drop n=<count>`.

Trace marker, one per change:

```
@RC stage=<ROBCO_STAGE_NAME> ev=audio op=<tone|stop> hz=<n> vol=<n> t_ms=<ms>
```

`t_ms` is the audio clock: it starts at `tick_ms()` in `audio_init()` and follows the system timer, updated each tick. Each change carries the time it was due. A new tone or a stop request takes effect at the next tick, so its time is that tick.

Sharp edges:

| Edge | Detail |
|---|---|
| Tick resolution. | Changes happen on ticks. At 100 Hz a 25 ms note sounds for 30 ms. Use a faster tick for clicks. |
| The tick must run first. | `audio_init()` checks it. Calls before a successful init return `AUDIO_ERR_NO_INIT`. |
| One voice. | A new sound replaces the old one. There is no mixing. |
| `audio_play()` keeps the pointer. | Do not pass a stack array that goes out of scope while it plays. |
| Late ticks skip notes. | A tick delayed past several steps reports each change in order. After 16 in one tick, the rest are skipped and only the final state counts. |
| Square wave only. | Volume changes the duty cycle, which also changes the tone colour. |
| PWM on real hardware. | The probe writes RNG1 with the PWM clock running. If it reads back wrong on a real board, the backend is `trace`: force PWM to try anyway. |

### Wiring

The analogue jack on the Pi 1, 2 and 3 already has a filter. On the Pi Zero, add one on the header pin:

| Part | Value |
|---|---|
| Series resistor from GPIO 18 | 270 ohm |
| Capacitor to ground after it | 33 nF (low-pass near 18 kHz) |
| DC-blocking capacitor to the amplifier | 10 uF |

For a piezo buzzer, connect it from the pin through a 100 ohm resistor to ground. Do not drive a speaker straight from a GPIO pin: use an amplifier or a transistor.

From `examples/audio/main.c`:

```c
static const audio_note_t arpeggio[] = { { 523, 120 }, { 659, 120 }, { 784, 200 } };

irq_init();
tick_init(100);
irq_cpu_enable();
audio_init();
audio_beep();
audio_wait();
audio_play(arpeggio, 3, 0);
audio_wait();
audio_play_rtttl("jingle:d=8,o=5,b=200:c,g,e,p,4c6,a,4g.", 0);
audio_wait();
```
