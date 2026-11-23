# Audio

This page covers audio: square-wave tones, note sequences, short tunes and 8-bit PCM samples. Error, init and context rules follow the [Conventions](../API.md#conventions).

## Audio (`audio.h`)

`audio.h` plays square-wave tones, note sequences and short tunes: a power-on beep, key clicks, simple jingles. It also plays 8-bit unsigned mono PCM. It has two backends. `pwm` drives the PWM block. `trace` prints one marker per change, for tests and for boards where PWM does not work. Tones play from a tick callback. PCM plays from the system timer compare channel 3 IRQ, one IRQ per sample. Every call returns at once. Nothing is allocated.

Init order: after step 10 of the [initialisation order](../API.md#initialisation-order). Call `audio_init()` once `irq_init()`, `tick_init(hz)` and `irq_cpu_enable()` have run. It takes one of the four tick callback slots and the `IRQ_TIMER3` handler.

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
| `AUDIO_ERR_PIN` | `-12` | `audio_init()` could not switch a pin to its PWM function |
| `AUDIO_HZ_MIN`, `AUDIO_HZ_MAX` | `20u`, `20000u` | Accepted tone range. `0` is a rest. |
| `AUDIO_PCM_RATE_MIN`, `AUDIO_PCM_RATE_MAX` | `4000u`, `22050u` | Accepted PCM sample rate |
| `AUDIO_PCM_LEN_MAX` | `0x7fffffffu` | Longest PCM buffer in bytes |
| `AUDIO_PCM_PWM_RANGE` | `256u` | PWM range in PCM mode: a 37.5 kHz carrier at 9.6 MHz |
| `AUDIO_PCM_NONE`, `_SAMPLE`, `_END` | `0` to `2` | `audio_pcm_poll()` results |
| `AUDIO_LOOP` | `1u` | Restart at the first step after the last |
| `AUDIO_PRIORITY` | `2u` | Replace what plays now |
| `AUDIO_BACKEND_AUTO`, `_PWM`, `_TRACE` | `0`, `1`, `2` | Backend choice |
| `AUDIO_PROBE_SKIPPED`, `_OK`, `_ABORT`, `_IGNORED` | `0` to `3` | Probe result |
| `AUDIO_PIN_NONE` | `-1` | Channel not used |
| `AUDIO_CLK_SRC_OSC`, `AUDIO_CLK_SRC_PLLD` | `1u`, `6u` | Clock manager source: 19.2 MHz oscillator, 500 MHz PLLD |
| `AUDIO_PWM_CLOCK_TARGET` | `9600000u` | PWM clock the divisor aims for |
| `AUDIO_PWM_DIVI_MIN`, `AUDIO_PWM_DIVI_MAX` | `2u`, `4095u` | Divisor limits |

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
| `ROBCO_AUDIO_PIN0`, `ROBCO_AUDIO_FN0` | per board, see below | Pin and `GPIO_FN_*` code for PWM channel 1. Set both. The build stops for a pin outside 0 to 53 or a code above 7, and the same holds for channel 2. |
| `ROBCO_AUDIO_PIN1`, `ROBCO_AUDIO_FN1` | none | Pin and code for PWM channel 2. Only read when `ROBCO_AUDIO_PIN0` is set. Leave it out for mono. |
| `ROBCO_AUDIO_CLOCK_SRC` | `AUDIO_CLK_SRC_OSC` | PWM clock source. Any other value than the two `AUDIO_CLK_SRC_*` stops the build. |
| `ROBCO_AUDIO_VOLUME` | `100` | Start volume in percent. The build stops above `100`. |
| `ROBCO_AUDIO_BEEP_HZ`, `ROBCO_AUDIO_BEEP_MS` | `1000`, `80` | The standard beep. The build stops when the frequency is outside `AUDIO_HZ_MIN` to `AUDIO_HZ_MAX` or the length is `0`. |
| `ROBCO_AUDIO_TUNE_MAX` | `64` | Notes in the `audio_play_rtttl()` buffer. The call also puts that many notes (8 bytes each) on the stack. |
| `ROBCO_AUDIO_TRACE_QUEUE` | `64` | Trace records held between two `audio_poll()` calls |
| `ROBCO_STAGE_NAME` | `"robco"` | `stage=` field of the trace markers |

Default pins when `ROBCO_AUDIO_PIN0` is not set:

| Board | Channel 1 | Channel 2 |
|---|---|---|
| Pi Zero, Zero W, Zero 2 W | GPIO 18, ALT5 | none |
| Other BCM2835 and BCM2836 boards (Pi 1, Pi 2) | GPIO 40, ALT0 | GPIO 45, ALT0 |
| Other BCM2837 boards (Pi 3) | GPIO 40, ALT0 | GPIO 41, ALT0 |

The Zero is detected from `board_get()->board_rev`: a new-style code (bit 23 set) with board type `0x09`, `0x0c` or `0x12` in bits 11:4. Old-style codes never count as a Zero. QEMU `raspi0` reports a Zero, so it uses GPIO 18.

The Pi Zero has no audio jack. Its PWM comes out on the header: GPIO 18 at ALT5, or GPIO 12 and 13 at ALT0 for two channels. Add a filter or a piezo there, see [Wiring](#wiring).

### Functions

```c
int audio_select(int backend);
int audio_set_pins(int pin0, unsigned fn0, int pin1, unsigned fn1);
void audio_get_pins(int *pin0, unsigned *fn0, int *pin1, unsigned *fn1);
```

Runtime overrides. Call them before `audio_init()` succeeds: afterwards `audio_select()` and `audio_set_pins()` return `AUDIO_ERR_STATE`. `audio_select()` overrides `ROBCO_AUDIO_BACKEND` and returns `AUDIO_ERR_ARG` for an unknown backend. `audio_set_pins()` overrides the macros and the board default. `pin0` carries PWM channel 1, `pin1` channel 2, and `AUDIO_PIN_NONE` turns a channel off. It returns `AUDIO_ERR_ARG` when a pin is not `AUDIO_PIN_NONE` or below `GPIO_PIN_COUNT`, or a function code is above `7`. The code is not checked against the pin: pick an alternate function that routes PWM to that pin. `audio_get_pins()` reads the pins in use, or the ones `audio_init()` will use. Any pointer may be `NULL`.

```c
int audio_init(void);
```

Checks the order, registers the tick callback and picks a backend. Returns `0` or an error, in this order:

| Check | Error |
|---|---|
| `irq_init()` has run | `AUDIO_ERR_NO_IRQ` |
| CPU IRQs are enabled | `AUDIO_ERR_NO_TICK` |
| `tick_count()` moves within 1.1 s of the system timer | `AUDIO_ERR_NO_TICK`. At `TICK_HZ_MIN` the wait can take a second. |
| A tick slot is free | `AUDIO_ERR_TICK_FULL` |

A failed check changes nothing, so you can retry. A call after a backend is installed returns `0`. `audio_init()` also registers the `IRQ_TIMER3` handler for PCM. If another handler holds it, `audio_init()` still succeeds and `audio_play_pcm()` returns `AUDIO_ERR_NO_IRQ`.

Backend selection:

| Choice | Probe | Backend | Returns |
|---|---|---|---|
| Auto | `ok` | `pwm` | `0` |
| Auto | `abort` or `ignored` | `trace` | `0` |
| PWM | `ok` or `ignored` | `pwm` | `0` |
| PWM | `abort` | `trace` | `AUDIO_ERR_NO_PWM` |
| Auto or PWM, a pin mux call fails | `ok` or `ignored` | `trace` | `AUDIO_ERR_PIN` |
| Trace | `skipped` | `trace` | `0` |

The probe reads the PWM CTL and RNG1 registers and the clock manager PWMCTL and PWMDIV registers with `mmio_probe()`. If any read aborts, or the board has no peripheral base, the result is `abort` and nothing is written. Otherwise it starts the PWM clock. If the clock BUSY bit does not settle within 1 ms, the result is `ignored`. Then it writes `0x0000A5A5` to RNG1, reads it back and restores the old value. A different value gives `ignored`. `AUDIO_ERR_NO_PWM` and `AUDIO_ERR_PIN` still leave audio usable: the trace backend is installed, so check `audio_backend()`. A pin mux call fails only when the GPIO block has no address, since the pins are checked before.

| Board | QEMU result |
|---|---|
| `raspi0`, `raspi1ap` | `abort`: QEMU has no PWM and the read aborts |
| `raspi2b`, `raspi3b` | `ignored`: reads return 0 and writes are dropped |

```c
const char *audio_backend(void);
int audio_probe_result(void);
const char *audio_probe_name(int result);
```

`audio_backend()` returns `"pwm"`, `"trace"`, or `"none"` before `audio_init()`. `audio_probe_result()` returns the `AUDIO_PROBE_*` code. `audio_probe_name()` turns it into `"ok"`, `"abort"`, `"ignored"` or `"skipped"`. Unknown values give `"skipped"`.

```c
int audio_tone(uint32_t hz, uint32_t ms);
int audio_beep(void);
```

Start one tone and return. `hz` `0` is a rest of `ms`. `ms` `0` does nothing. Otherwise both replace whatever plays. `audio_beep()` plays `ROBCO_AUDIO_BEEP_HZ` for `ROBCO_AUDIO_BEEP_MS`. `audio_tone()` returns `0`, `AUDIO_ERR_RANGE` or `AUDIO_ERR_NO_INIT`. It checks the range first, so a bad `hz` gives `AUDIO_ERR_RANGE` even before init. `audio_beep()` returns `0` or `AUDIO_ERR_NO_INIT`.

```c
int audio_play(const audio_note_t *seq, size_t count, unsigned flags);
```

Plays `count` steps in order. Steps with `ms` `0` are skipped. With `AUDIO_LOOP` the sequence repeats until `audio_stop()` or a replacement. Without `AUDIO_PRIORITY` the call returns `AUDIO_ERR_BUSY` while something plays or waits to start. A pending stop does not count. With `AUDIO_PRIORITY`, the new sequence replaces the old one at the next tick. The library keeps the pointer: `seq` must stay valid until playback ends.

Returns `0` or, in this order, `AUDIO_ERR_NO_INIT`, `AUDIO_ERR_ARG` (`seq` `NULL` with `count` above `0`, or unknown flags), `AUDIO_ERR_RANGE` or `AUDIO_ERR_BUSY`. Every `hz` is checked before anything changes. `count` `0` returns `0` and plays nothing. It does not stop the current sound, even with `AUDIO_PRIORITY`.

```c
int audio_play_pcm(const uint8_t *data, size_t len, uint32_t rate_hz, unsigned flags);
```

Plays `len` bytes of 8-bit unsigned mono PCM at `rate_hz` in the background. 128 is silence. The library does not copy: `data` must stay valid until playback ends, or until `audio_stop()` or a call that replaces the sound returns. Such a call stops reading the buffer and silences the PCM before it returns, so the buffer is yours again. The new sound still starts at the next tick. Flags are those of `audio_play()`. With `AUDIO_LOOP` the buffer repeats until `audio_stop()` or a replacement. PCM and tones are one voice: each replaces the other under the same `AUDIO_PRIORITY` rule, and `audio_busy()`, `audio_stop()`, `audio_wait()` and `audio_poll()` work the same. Playback starts at the next tick, like a tone.

Returns `0` or, in this order, `AUDIO_ERR_NO_INIT`, `AUDIO_ERR_ARG` (`data` `NULL`, `len` `0` or above `AUDIO_PCM_LEN_MAX`, or unknown flags), `AUDIO_ERR_RANGE` (`rate_hz` outside `AUDIO_PCM_RATE_MIN` to `AUDIO_PCM_RATE_MAX`), `AUDIO_ERR_NO_IRQ` (another handler holds `IRQ_TIMER3`) or `AUDIO_ERR_BUSY`.

How it plays: the start arms compare channel 3. Channels 0 and 2 belong to the GPU and channel 1 to the tick. Each IRQ reads the clock and outputs sample `elapsed_us * rate / 1000000`, then arms the compare for the next sample. The position comes from the clock, not from a count of IRQs, so nothing drifts and a late IRQ skips samples instead of slowing the sound. When the compare value is already in the past, the handler polls again, and after 4 misses it arms with `timer_oneshot_arm()`. A non-looped play ends at the first IRQ after `len / rate` seconds.

Each sample is scaled by the volume around the midpoint: `128 + (s - 128) * vol / 100`, with C integer division. A volume change applies from the next sample. On the PWM backend the PWM runs at a fixed range of `AUDIO_PCM_PWM_RANGE`, a 37.5 kHz carrier from the 9.6 MHz PWM clock, and each IRQ writes the duty. A PCM start switches the PWM to that range. A tone sets its own range again. A stop, a replacement or the end of a play sets the duty to `0`, the same silence as tones.

CPU cost: one IRQ per sample, up to 22050 a second. The handler does 64-bit multiplies but no 64-bit divide. A looped buffer adds a 32-bit modulo per sample after its first pass. On the trace backend the call also computes the CRC-32 of `data` in the caller's context. The handler cost on a real ARM1176 is not measured.

Not done: no mixing, no DMA, no FIFO, 8-bit mono only.

```c
uint32_t audio_pcm_samples(uint32_t *play);
```

Returns the samples output since `audio_init()`, the same IRQs that `n=` counts. If `play` is not `NULL`, it gets the samples output by the current or last play, queued buffers included. A new play starts that count at `0` on its tick. Tests use it to check that no sample is read after a stop. Main or IRQ context.

```c
int audio_pcm_queue(const uint8_t *data, size_t len);
int audio_pcm_can_queue(void);
```

`audio_pcm_queue()` queues the next buffer behind the PCM that plays. It starts with no gap when the current buffer ends, at the same rate and volume. The first sample of the queued buffer is due one sample period after the last sample of the current one, on the same clock grid: the engine does not restart its time base, so many chunks in a row do not drift. One buffer can wait. The call works while the play still waits for its tick. The library does not copy: `data` must stay valid until that buffer has finished playing, or until `audio_stop()` or a call that replaces the sound returns. Those calls drop the waiting buffer and stop reading both buffers before they return.

Returns `0` or, in this order, `AUDIO_ERR_NO_INIT`, `AUDIO_ERR_ARG` (`data` `NULL`, `len` `0` or above `AUDIO_PCM_LEN_MAX`), `AUDIO_ERR_STATE` (no PCM plays or waits to start, or the PCM loops) or `AUDIO_ERR_BUSY` (a buffer already waits). On the trace backend the call computes the CRC-32 of `data`, after the state checks.

`audio_pcm_can_queue()` returns `1` when `audio_pcm_queue()` would accept a buffer now: PCM plays or waits to start, it does not loop, and no buffer waits. When it turns `1` again after a queue call, the queued buffer has taken over, and the buffer that played at the time of that call is free.

Underrun: when the current buffer ends and no buffer waits, the play ends as a single play does, with `op=pcm_end`. A later `audio_pcm_queue()` returns `AUDIO_ERR_STATE`. Start again with `audio_play_pcm()`. The sound then has a gap.

`audio_busy()` stays non-zero across a switch. The per-play count of `audio_pcm_samples()` runs from `audio_play_pcm()` over every queued buffer that follows.

Streaming with two buffers, refilled from a file or a generator:

```c
static uint8_t buf[2][4096];

void stream(void)
{
    size_t n = fill(buf[0], sizeof buf[0]);
    int cur = 1;

    audio_play_pcm(buf[0], n, 8000, 0);
    while ((n = fill(buf[cur], sizeof buf[cur])) != 0) {
        /* Waits until the other buffer has taken over. */
        while (!audio_pcm_can_queue() && audio_busy()) {
            audio_poll();
        }
        if (audio_pcm_queue(buf[cur], n) == AUDIO_ERR_STATE) {
            audio_play_pcm(buf[cur], n, 8000, 0);   /* underrun: restart */
        }
        cur ^= 1;
    }
    audio_wait();
}
```

`fill()` is yours: it writes up to `max` bytes and returns how many, `0` at the end. It writes into the buffer that played before. It is free: the queue call that put it in has returned, and `audio_pcm_can_queue()` turned `1` after that.

```c
int audio_play_rtttl(const char *text, unsigned flags);
```

Parses an RTTTL tune into an internal buffer of `ROBCO_AUDIO_TUNE_MAX` notes and plays it. Same flags as `audio_play()`. Returns `0`, `AUDIO_ERR_NO_INIT`, `AUDIO_ERR_ARG` (unknown flags or `text` `NULL`), `AUDIO_ERR_PARSE`, `AUDIO_ERR_RANGE`, `AUDIO_ERR_FULL` or `AUDIO_ERR_BUSY`. A tune with no notes returns `0` and plays nothing. The text may be freed once the call returns.

```c
void audio_stop(void);
int audio_busy(void);
void audio_set_volume(unsigned percent);
```

`audio_stop()` stops PCM output before it returns. A tone or sequence stops at the next tick. Before `audio_init()` it does nothing. `audio_busy()` is non-zero while a tone, rest, sequence or PCM buffer plays, or a request (a stop included) waits for the tick. After `audio_stop()` it turns `0` at the next tick, even for PCM. `audio_set_volume()` sets the PWM duty cycle from `0` (silent) to `100` (50%, the loudest square wave). It applies from the next step change, or from the next PCM sample. Values above 100 count as 100. The trace backend records it in `vol=`.

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
| `b` | Beats per minute, `1` to `900`. Default `63`. A note of duration `d` lasts `240000 / (b * d)` ms, rounded down. |
| Numbers | One to four decimal digits |
| `letter` | `c` to `g`, `a`, `b`, any case. `#` raises a semitone. `e#` and `b#` are errors. |
| `p` | A rest. It accepts a duration, dot and octave like a note. |
| `.` | Dotted: 1.5 times the length, rounded down. One dot only, before or after the octave. |

Setting keys are case-insensitive. A setting may repeat: the last one wins. Spaces, tabs, carriage returns and newlines are allowed around `:`, `=` and `,`, but not inside a note. A trailing `,` after the last note is an error. Frequencies come from equal temperament with A4 = 440 Hz, rounded to whole hertz. `c0` to `d#0` fall below 20 Hz and give `AUDIO_ERR_RANGE`. Example: `"terminal:d=8,o=5,b=200:c,g,e,p,4c6,a,4g."`.

### Pure functions

These are in `src/lib/` and tested on the host.

```c
uint32_t audio_note_hz(unsigned semitone, unsigned octave);
```

Frequency of semitone `0` (C) to `11` (B) in octave `0` to `8`, rounded to whole hertz. Integer maths from an octave 4 table in millihertz. `0` when out of range.

```c
int audio_rtttl_parse(const char *text, audio_note_t *out, size_t max, size_t *err_pos);
```

Parses RTTTL into `out` and returns the note count, or `AUDIO_ERR_ARG` (`text` `NULL`), `AUDIO_ERR_PARSE`, `AUDIO_ERR_RANGE` or `AUDIO_ERR_FULL`. On error `*err_pos` gets the byte offset of the problem, if `err_pos` is not `NULL`. For `AUDIO_ERR_RANGE` and `AUDIO_ERR_FULL` it points at the start of the note, before any leading space. With `out` `NULL` it only counts, up to `max`.

```c
int audio_seq_start(audio_seq_t *s, const audio_note_t *notes, size_t count,
                    unsigned flags, uint32_t now_ms);
void audio_seq_stop(audio_seq_t *s);
int audio_seq_poll(audio_seq_t *s, uint32_t now_ms, uint32_t *at_ms);
```

The sequencer the tick uses. Treat the `audio_seq_t` fields as private, except `s->hz`, which is what should sound. `audio_seq_start()` returns `1` if something plays, `0` if every step is empty. `audio_seq_poll()` moves past one due step change, stores when it was due in `*at_ms` and returns `1`. Call it until it returns `0`. A looped sequence that falls a whole loop behind skips the full loops at once. Times wrap at 2^32 ms.

```c
int audio_pcm_check(const uint8_t *data, size_t len, uint32_t rate_hz, unsigned flags);
void audio_pcm_start(audio_pcm_t *p, const uint8_t *data, size_t len, uint32_t rate_hz,
                     unsigned flags, uint32_t now_us);
int audio_pcm_poll(audio_pcm_t *p, uint32_t now_us, uint32_t *pos);
int audio_pcm_chain(audio_pcm_t *p, const uint8_t *data, size_t len);
uint32_t audio_pcm_next_us(const audio_pcm_t *p);
uint8_t audio_pcm_scale(uint8_t s, unsigned vol);
uint32_t audio_pcm_duty(uint8_t s, uint32_t rng);
```

The PCM engine the IRQ uses. Treat the `audio_pcm_t` fields as private, except `n`, the samples output over every chained buffer, and `last`, the last index output in the current buffer. `audio_pcm_check()` returns `0`, `AUDIO_ERR_ARG` or `AUDIO_ERR_RANGE` with the rules of `audio_play_pcm()`. `audio_pcm_start()` puts sample `0` at `now_us` and does the 64-bit divides, so the IRQ does not. `audio_pcm_poll()` returns `AUDIO_PCM_SAMPLE` and stores the index in `*pos` when a sample not output yet is due, `AUDIO_PCM_NONE` before that, and `AUDIO_PCM_END` once a non-looped buffer has played. `audio_pcm_chain()` puts `data` behind the current buffer. When the position passes the end, the engine switches to it and the position continues at `pos - len`, with the same origin time. A poll that comes late lands at the right index of the new buffer, and one that passes the new buffer's end too returns `AUDIO_PCM_END`. It returns `0`, `AUDIO_ERR_STATE` (not active, or looped) or `AUDIO_ERR_BUSY` (a buffer waits). `audio_pcm_next_us()` says when the next sample is due. Times wrap at 2^32 us. The engine moves its origin forward by whole sample groups every 2^26 us, so a loop can run for hours without drift. `audio_pcm_scale()` applies the volume, capped at `100`. `audio_pcm_duty()` returns `s * rng / 256`.

```c
int audio_pwm_calc(uint32_t src_hz, uint32_t hz, unsigned vol, audio_pwm_cfg_t *out);
```

Fills `divi`, `clock_hz`, `rng` and `dat` of an `audio_pwm_cfg_t`. The divisor is `src_hz / 9600000` rounded to nearest and clamped to `2` to `4095`. Then `clock_hz = src_hz / divi`, `rng = clock_hz / hz` rounded to nearest, and `dat = (rng / 2 * vol + 50) / 100`. `vol` above 100 counts as 100. Returns `0`, `AUDIO_ERR_ARG` (`out` `NULL` or `src_hz` below `2`) or `AUDIO_ERR_RANGE` (`hz` out of range, or `rng` below `2`).

| Source | `hz` | `divi` | `clock_hz` | `rng` | `dat` at 100% |
|---|---|---|---|---|---|
| Oscillator, 19200000 | 440 | `2` | `9600000` | `21818` | `10909` |
| PLLD, 500000000 | 440 | `52` | `9615384` | `21853` | `10926` |

### Context rules

| Function | Context |
|---|---|
| `audio_select()`, `audio_set_pins()`, `audio_get_pins()`, `audio_init()` | Main context. `audio_init()` busy-waits for the tick. |
| `audio_tone()`, `audio_beep()`, `audio_play()`, `audio_stop()`, `audio_busy()`, `audio_set_volume()`, `audio_pcm_samples()` | Main or IRQ context. They mask IRQs for a few instructions. |
| `audio_play_rtttl()` | Main context: it parses with a buffer on the stack |
| `audio_play_pcm()`, `audio_pcm_queue()` | Main context: on the trace backend they compute a CRC over the whole buffer |
| `audio_pcm_can_queue()` | Main or IRQ context. It masks IRQs for a few instructions. |
| `audio_poll()`, `audio_wait()` | Main context only. They print with `k_printf()`. |

The trace backend never prints from an IRQ. The UART driver and `k_printf()` have no lock, so a print from IRQ context could interleave with the main flow. The tick and the PCM IRQ put each record in a queue of `ROBCO_AUDIO_TRACE_QUEUE` entries, and `audio_poll()` or `audio_wait()` prints them. When the queue is full, records are dropped and the next poll prints:

```
@RC stage=<ROBCO_STAGE_NAME> ev=audio_drop n=<count>
```

Trace marker, one per change:

```
@RC stage=<ROBCO_STAGE_NAME> ev=audio op=<tone|stop> hz=<n> vol=<n> t_ms=<ms>
```

`op=stop` means `hz=0`: the end of a sound, a rest or `audio_stop()`. A change from silence to silence prints nothing. `t_ms` is the audio clock: it starts at `tick_ms()` in `audio_init()` and follows the system timer, updated each tick. Each change carries the time it was due. A new tone or a stop request takes effect at the next tick, so its time is that tick.

PCM prints three markers. The trace backend runs the same engine with the same timer IRQ, so QEMU tests exercise the real IRQ path:

```
@RC stage=<ROBCO_STAGE_NAME> ev=audio op=pcm rate=<hz> len=<bytes> vol=<n> crc=<8 hex> t_ms=<ms>
@RC stage=<ROBCO_STAGE_NAME> ev=audio op=pcm_next len=<bytes> crc=<8 hex> t_ms=<ms>
@RC stage=<ROBCO_STAGE_NAME> ev=audio op=pcm_end n=<count> last=<index> t_ms=<ms>
```

`op=pcm` is the start. `crc` is the CRC-32 of the data, as `crc32()` computes it. `op=pcm_next` means a queued buffer took over, with its length and CRC. Its `t_ms` is the start time plus the system timer time until the IRQ that switched. `op=pcm_end` is the natural end, printed once, when the last buffer ends: `n` counts the IRQs that output a sample over the whole play, queued buffers included, and `last` is the last index output, in the last buffer. Its `t_ms` is the start time plus the system timer time the play took. A stop or a replacement prints the `op=stop` line instead, at the next tick. The PCM output itself stops when the call returns.

Sharp edges:

| Edge | Detail |
|---|---|
| Tick resolution. | Changes happen on ticks. At 100 Hz a 25 ms note sounds for 30 ms. Use a faster tick for clicks. |
| Requests start on the next tick. | A second `audio_tone()` or `audio_beep()` before the next tick replaces the first one, which never sounds. Two key clicks in one tick period give one click. |
| The tick must run first. | `audio_init()` checks it. Calls before a successful init return `AUDIO_ERR_NO_INIT`. |
| One voice. | A new sound replaces the old one. There is no mixing. |
| `audio_play()` keeps the pointer. | Do not pass a stack array that goes out of scope while it plays. |
| Late ticks skip notes. | A tick delayed past several steps reports each change in order, at its due time. After 16 in one tick, the rest are skipped and only the final state is output. |
| Square wave tones. | Volume changes the duty cycle, which also changes the tone colour. PCM volume scales the samples instead. |
| One IRQ per PCM sample. | At 22050 Hz that is 22050 IRQs a second, and other IRQs wait behind each one. Use the lowest rate that sounds right. |
| `audio_play_pcm()` keeps the pointer. | The buffer must stay valid until the play ends, or until `audio_stop()` or a call that replaces the sound returns. |
| `audio_pcm_queue()` keeps the pointer. | Two buffers can be in use: the one that plays and the one that waits. Reuse a buffer only once `audio_pcm_can_queue()` turns `1` after the queue call that followed it. |
| One buffer waits. | A ROM that refills late lets the sound end. The next chunk then needs `audio_play_pcm()`, with a gap. |
| Fewer PCM IRQs on QEMU. | Under QEMU 10.1 on a macOS host, the guest got about one system timer IRQ per millisecond: 81 IRQs for 800 samples at 8000 Hz, and 1243 for one second at 22050 Hz. We did not look for the cause or measure other hosts. On QEMU, `n` can be far below `len` while the play still ends on time. The tests therefore do not assert an IRQ per sample. |
| PWM on real hardware. | The probe writes RNG1 with the PWM clock running. If it reads back wrong on a real board, auto picks `trace`: force PWM to try anyway. |
| Pin mux errors are silent. | `audio_init()` ignores a `gpio_set_function()` failure on the PWM backend. |

Pending hardware validation: QEMU has no PWM, so the `pwm` backend, the default pins and the sound itself are only checked on real boards. See [hardware validation](../../HARDWARE_VALIDATION.md).

| Check | Boards | Look for |
|---|---|---|
| Probe and backend | Pi Zero or Pi 1, Pi 2, Pi 3 or Zero 2 W | `ev=backend name=pwm probe_pwm=ok` from [`examples/audio`](../../../examples/audio/main.c) |
| Sound on the jack or GPIO 18 | Same | Beep, arpeggio, tune, looped alarm and the cut-short 440 Hz tone. Press `b`, `s`, `t`, `l`, `i` or `a` on the serial console to replay. |
| PCM sound | Same | A 100 ms rising chirp. Press `p` to replay it. The PCM IRQ cost on the ARM1176 is not measured yet. |
| Queued PCM | Same | Three chunks of the chirp play as one 300 ms sound, with no click between chunks. Press `c` to replay it. |

### Wiring

The analogue jack on the Pi 1, 2 and 3 already has a filter. On the Pi Zero, add one on the header pin:

| Part | Value |
|---|---|
| Series resistor from GPIO 18 | 270 ohm |
| Capacitor to ground after it | 33 nF (low-pass near 18 kHz) |
| DC-blocking capacitor to the amplifier | 10 uF |

For a piezo buzzer, connect it from the pin through a 100 ohm resistor to ground. Do not drive a speaker straight from a GPIO pin: use an amplifier or a transistor.

Adapted from [`examples/audio/main.c`](../../../examples/audio/main.c):

```c
static const audio_note_t arpeggio[] = { { 523, 120 }, { 659, 120 }, { 784, 200 } };
static const char jingle[] = "terminal:d=8,o=5,b=200:c,g,e,p,4c6,a,4g.";

irq_init();
tick_init(100);
irq_cpu_enable();
audio_init();
audio_beep();
audio_wait();
audio_play(arpeggio, 3, 0);
audio_wait();
audio_play_rtttl(jingle, 0);
audio_wait();
audio_play_pcm(pcm_buf, 800, 8000, 0);   /* filled at run time */
audio_wait();
audio_play_pcm(pcm_buf, 800, 8000, 0);   /* three chunks as one sound */
audio_pcm_queue(pcm_buf + 800, 600);
while (!audio_pcm_can_queue()) {
    audio_poll();
}
audio_pcm_queue(pcm_buf + 1400, 1000);
audio_wait();
```

The example also has a `pwm` variant built with `-DROBCO_AUDIO_BACKEND=1`.
