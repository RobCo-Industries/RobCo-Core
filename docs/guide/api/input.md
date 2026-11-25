# Input

This page covers the input event queue with its GPIO, console and script backends, and the mini UART that carries the script channel. Rules that apply to every header are in [Conventions](../API.md#conventions).

## Input events (`input.h`)

`input.h` turns GPIO buttons, a rotary dial and serial lines into one stream of events. Backends push events into a fixed queue from IRQ or main context. The main loop reads them with `input_poll()` or `input_wait()`. Serial input lets a person or a test drive a product in QEMU. A test can also drive the GPIO buttons and the dial in QEMU from outside the guest: see [Testing with real lines](#testing-with-real-lines).

| Part | File | Touches hardware |
|---|---|---|
| Ring queue | `src/lib/input_queue.c` | No |
| Queue, key and dial state | `src/lib/input_core.c` | No |
| Debouncer | `src/lib/input_debounce.c` | No |
| Quadrature decoder | `src/lib/input_quad.c` | No |
| ANSI/VT parser | `src/lib/input_ansi.c` | No |
| Queue, key state, repeat | `src/drv/input.c` | IRQ mask, system timer, tick |
| GPIO backend | `src/drv/input_gpio.c` | GPIO, tick |
| Console backend | `src/drv/input_serial.c` | PL011 |
| Script backend | `src/drv/input_script.c` | Mini UART |

### Events

```c
typedef struct {
    uint8_t type;       /* INPUT_EV_* */
    uint8_t source;     /* INPUT_SRC_* */
    uint16_t code;      /* INPUT_KEY_*, dial index or character */
    int32_t value;
    uint32_t t_ms;      /* timer_now_ms() when pushed */
} input_event_t;
```

| Type | Value | `code` | `value` |
|---|---|---|---|
| `INPUT_EV_KEY` | `1` | `INPUT_KEY_*` | `INPUT_PRESS` (1), `INPUT_RELEASE` (0), `INPUT_REPEAT` (2) |
| `INPUT_EV_DIAL` | `2` | Dial index, in the order of `input_gpio_add_dial()` | Signed detent steps, positive is clockwise |
| `INPUT_EV_CHAR` | `3` | A printable character, Tab or another control code | `0` |

| Source | Value |
|---|---|
| `INPUT_SRC_GPIO` | `0` |
| `INPUT_SRC_SERIAL` | `1` |
| `INPUT_SRC_SCRIPT` | `2` |

| Key | Value | | Key | Value |
|---|---|---|---|---|
| `INPUT_KEY_NONE` | `0` | | `INPUT_KEY_F1` | `7` |
| `INPUT_KEY_UP` | `1` | | `INPUT_KEY_F2` | `8` |
| `INPUT_KEY_DOWN` | `2` | | `INPUT_KEY_F3` | `9` |
| `INPUT_KEY_LEFT` | `3` | | `INPUT_KEY_F4` | `10` |
| `INPUT_KEY_RIGHT` | `4` | | `INPUT_KEY_DIAL_PUSH` | `11` |
| `INPUT_KEY_SELECT` | `5` | | `INPUT_KEY_COUNT` | `12` |
| `INPUT_KEY_BACK` | `6` | | | |

| Error | Value | Meaning |
|---|---|---|
| `INPUT_ERR_FULL` | `-1` | Queue full. The event is dropped and counted. |
| `INPUT_ERR_ARG` | `-2` | Bad argument |
| `INPUT_ERR_NO_IRQ` | `-3` | `irq_init()` has not run |
| `INPUT_ERR_TICK` | `-4` | The tick does not run at the given rate |
| `INPUT_ERR_SPACE` | `-5` | A fixed table or the tick callback slots are full |
| `INPUT_ERR_STATE` | `-6` | Wrong call order, or already started |
| `INPUT_ERR_NO_UART` | `-7` | The UART the backend reads is not initialised |

### Init order

| Step | Call | Needs |
|---|---|---|
| 1 | `uart_init(115200)` | Board detection |
| 2 | `uart_mini_init(115200)` | Board detection. Only for the script backend. |
| 3 | `irq_init()`, `tick_init(hz)`, `irq_cpu_enable()` | See [Initialisation order](../API.md#initialisation-order) |
| 4 | `input_init()` | Nothing |
| 5 | `input_gpio_add_button()`, `input_gpio_add_dial()` | Nothing |
| 6 | `input_gpio_start(hz, debounce_ms)` | Steps 3 and 5. Same `hz` as `tick_init()`. |
| 7 | `input_set_repeat(delay_ms, rate_ms)` | Step 3, optional |
| 8 | `input_serial_start(mode)` | Step 1 |
| 9 | `input_script_start()` | Step 2 |

The GPIO backend, auto-repeat and the console backend in tick mode each take one of the `TICK_MAX_CALLBACKS` (4) tick callback slots. All three together leave one slot for the project.

### Queue and key state

```c
int input_init(void);
```

Empties the queue, clears the counters, the key state and the dial totals, sets the overflow policy back to `INPUT_DROP_NEWEST`, and stops a repeat in progress. It keeps the repeat delay and rate and the registered pollers. Returns `0`. Call it once before any backend starts.

```c
int input_push(const input_event_t *ev);
```

Copies the event into the queue and stamps `t_ms` with `timer_now_ms()`. For a KEY event it also updates the key state and the repeat timer. For a DIAL event it adds the steps to the dial total. Both happen before the queue sees the event, so they hold even when an event is dropped. When the queue holds `INPUT_QUEUE_LEN` (32) events, the overflow policy decides. Returns `0`, `INPUT_ERR_FULL` under `INPUT_DROP_NEWEST` (the new event is dropped and counted, the queued ones stay), or `INPUT_DROPPED_OLDEST` (1) under `INPUT_DROP_OLDEST` (the new event is queued, the oldest one is dropped and counted). A non-zero return always means an event was lost. Safe from any context. Backends call it. A project can call it too, for example to inject events from its own driver.

```c
int input_poll(input_event_t *ev);
```

Runs the main-loop backends (the console backend in `INPUT_SERIAL_MAIN` mode and the script backend), then takes the oldest event. Returns `1` with the event in `*ev`, or `0`. Never blocks. Main context only. A nested call from inside a poller skips the pollers.

```c
#define INPUT_WAIT_FOREVER  0xffffffffu
int input_wait(input_event_t *ev, uint32_t timeout_ms);
```

Calls `input_poll()` until an event comes or `timeout_ms` passes. Returns `1` or `0`. Between checks it sleeps in `wfi` when CPU IRQs are on and the tick has fired at least once, so a serial byte waits at most one tick period. With IRQs masked or no tick it spins. Main context only.

```c
unsigned input_pending(void);
void input_flush(void);
void input_stats(uint32_t *pushed, uint32_t *dropped);
```

`input_pending()` counts queued events. `input_flush()` drops them without changing the counters or the key state. `input_stats()` stores the events accepted and dropped since `input_init()`. Either pointer may be 0. Repeat events count too.

```c
int input_key_down(unsigned code);
```

Non-zero while any source holds `code`. A release clears only the hold of its own source, so a key pressed on the console and on a button stays down until both release it. The state follows the KEY events pushed, even those the queue dropped. Sources above 3 are not tracked.

```c
int input_set_repeat(uint32_t delay_ms, uint32_t rate_ms);
```

Turns on auto-repeat. While a key is held, the first `INPUT_REPEAT` event comes `delay_ms` after the press, then one every `rate_ms`. It has the source of the press. Only the last key pressed repeats. It goes on while any source still holds that key. `delay_ms` 0 turns it off, which is the default. It runs from a tick callback, registered on the first call with a non-zero delay. Returns `0`, `INPUT_ERR_ARG` for a non-zero delay with a zero rate, or `INPUT_ERR_SPACE` when no tick slot is free. Each call stops a repeat in progress. After a stall it goes on from the current time instead of sending a burst.

```c
int input_add_poller(void (*fn)(void));
const char *input_key_name(unsigned code);
```

`input_add_poller()` adds a function that `input_poll()` runs in main context. Returns `0`, `INPUT_ERR_ARG` for a null function, or `INPUT_ERR_SPACE` past four pollers. Backends use it. There is no way to remove one. `input_key_name()` returns `"up"`, `"down"`, `"left"`, `"right"`, `"select"`, `"back"`, `"f1"` to `"f4"` or `"dial_push"`, or 0 for an unknown code.

```c
int32_t input_dial_total(unsigned dial);
```

Returns the sum of the signed detent steps of DIAL events with code `dial` since `input_init()`. It counts the events the queue dropped, so a caller that reads it never loses steps. The sum wraps as a 32-bit two's-complement counter: keep the last value and take the difference. Returns `0` for a dial at or above `INPUT_DIAL_COUNT` (2, which covers `INPUT_GPIO_MAX_DIALS` and the script channel's dial 0). DIAL events with a higher code still reach the queue. Safe from any context.

```c
#define INPUT_DROP_NEWEST   0
#define INPUT_DROP_OLDEST   1
void input_set_overflow(int policy);
unsigned input_dropped(void);
int input_room(unsigned n);
```

`input_set_overflow()` picks what a full queue does. `INPUT_DROP_NEWEST`, the default, refuses the new event. `INPUT_DROP_OLDEST` drops the oldest queued event and appends the new one, so the queue keeps the latest 32. Other values change nothing. `input_init()` sets the default again. `input_dropped()` returns the events dropped since `input_init()` under either policy, the same count as `input_stats()`. `input_room(n)` returns non-zero when `n` more events fit with no loss under the current policy. A backend calls it before it takes data it cannot give back. All three are safe from any context.

| On overflow | `INPUT_DROP_NEWEST` | `INPUT_DROP_OLDEST` |
|---|---|---|
| Queued events | The oldest 32 stay | The newest 32 stay |
| `input_push()` returns | `INPUT_ERR_FULL` | `INPUT_DROPPED_OLDEST` |
| Key state, `input_key_down()` | Kept | Kept |
| Dial totals, `input_dial_total()` | Kept | Kept |
| `input_dropped()` | Counts the new event | Counts the old event |
| `pushed` in `input_stats()` | Not counted | Counted |
| Script answer | `err full` | `ok dropped` |

### GPIO buttons and dial

| Constant | Value | Meaning |
|---|---|---|
| `INPUT_GPIO_MAX_BUTTONS` | `16` | Buttons, dial push buttons included |
| `INPUT_GPIO_MAX_DIALS` | `2` | Dials |
| `INPUT_GPIO_NO_PIN` | `-1` | A dial without a push button |
| `INPUT_GPIO_GUARD_MS` | `100` | Length of the stuck-line guard |

```c
int input_gpio_add_button(unsigned pin, unsigned code, int active_level, unsigned pull);
```

Registers a button. `active_level` is `0` when the pressed button reads low (a button to ground with `GPIO_PULL_UP`) or `1`. `pull` is a `GPIO_PULL_*` code. Returns `0`, `INPUT_ERR_ARG` for a pin of 54 or more, `INPUT_KEY_NONE` or an unknown code, or a bad level or pull, `INPUT_ERR_SPACE` past 16 buttons, or `INPUT_ERR_STATE` after `input_gpio_start()`.

```c
int input_gpio_add_dial(unsigned pin_a, unsigned pin_b, int push_pin, unsigned per_detent);
```

Registers a quadrature dial with pull-ups on both pins, and an optional active-low push button with a pull-up that reports `INPUT_KEY_DIAL_PUSH`. The push button takes a button slot. `per_detent` is the number of A/B transitions per click: 1, 2 or 4. Swap `pin_a` and `pin_b` if the dial counts the wrong way. Returns `0`, `INPUT_ERR_ARG` for a bad pin, equal A and B pins or another `per_detent`, `INPUT_ERR_SPACE` past 2 dials or when the push button finds no button slot, or `INPUT_ERR_STATE` after `input_gpio_start()`.

```c
int input_gpio_start(uint32_t tick_hz, unsigned debounce_ms);
```

Sets the pins to input with their pulls, then samples every pin from a tick callback, once per tick. Buttons go through the debouncer with a stable time of `debounce_ms`, rounded up to whole ticks (1 to 255). Dials go through the quadrature decoder. Each sample that completes steps gives one DIAL event with their signed count: one step with 2 or 4 per detent, up to 7 with 1 per detent after missed states. Returns `0`, `INPUT_ERR_NO_IRQ` before `irq_init()`, `INPUT_ERR_TICK` when `tick_init(tick_hz)` does not return `0`, `INPUT_ERR_SPACE` when no tick slot is free, or `INPUT_ERR_STATE` on a second call. Main context.

#### Dial speed

The GPIO backend samples on the tick and takes no GPIO edge interrupt. QEMU models neither edge detection nor the GPIO interrupt (V19), so no test could cover that path. Sampling faster than the tick would need another system timer channel, and channels 0 and 2 belong to the GPU, 1 to the tick and 3 to PCM audio. A sample gap that hides one state between two seen states costs nothing. The decoder rules under [Pure modules](#pure-modules) say why.

Measured in QEMU at a 100 Hz tick, 4 transitions per detent, 20 detents each way per rate, 10 sweeps over all four boards, at a load average of 8 to 18 from other jobs:

| States | Every step counted | Steps lost |
|---|---|---|
| Equal | Down to 12 ms per state, 20.8 detents per second | 10 ms: 4 of 20 runs lost 2 to 4 |
| Rest 3 times longer | Down to 12 ms per state, 13.9 detents per second | 10 ms: 3 of 20 runs lost 2 |

At 15 and 12 ms with the rest 3 times longer, one run each lost one step while the guest's tick stalled for 32 and 55 ms.

The test `test_gpio_dial_at_the_limit` enforces 15 ms per state, 16.7 and 11.1 detents per second, a 3 ms margin. The decoder before these rules counted every step at 15 ms with equal states but lost one in 8 of 20 runs with the rest 3 times longer. These are QEMU numbers. A real hand and real contacts are not measured.

`tick.h` cannot report the running rate, so `input_gpio_start()` calls `tick_init(tick_hz)`. If the tick already runs with the same period, this returns `0` and changes nothing. If it does not run yet, the call starts it.

```c
int input_gpio_status(void);
const char *input_gpio_status_name(int status);
```

| Status | Name | Meaning |
|---|---|---|
| `INPUT_GPIO_OFF` | `off` | Not started, or the start failed |
| `INPUT_GPIO_CHECKING` | `checking` | The guard is still sampling |
| `INPUT_GPIO_ACTIVE` | `active` | Events flow |
| `INPUT_GPIO_DISABLED_STUCK` | `disabled_stuck` | Every button read active for the whole guard. The backend stopped. |

`input_gpio_status_name()` returns `"unknown"` for any other value.

The stuck-line guard samples for the first 100 ms of ticks (at least one tick) and pushes no event. If every registered button reads active at every sample, the lines are taken as not wired, as in QEMU where an undriven input reads 0 and active-low buttons look pressed. The backend then stops for good. Otherwise it turns active and reports a press for each button whose debounced state is pressed. With no button registered, the guard always passes.

### Console backend

```c
#define INPUT_SERIAL_MAIN   0
#define INPUT_SERIAL_TICK   1
#define INPUT_SERIAL_ESC_MS 100
int input_serial_start(int mode);
```

Reads the PL011 console through the ANSI parser. `INPUT_SERIAL_MAIN` reads from `input_poll()`. `INPUT_SERIAL_TICK` reads from a tick callback. Each call reads up to 64 bytes. One byte makes at most `INPUT_SERIAL_BYTE_EVENTS` (4) queued events: a printable byte makes one CHAR event, a key byte a press and a release, and a byte after a lone ESC can make BACK and a key. Under `INPUT_DROP_NEWEST` the backend reads a byte only when the queue has room for 4 events. The other bytes wait in the PL011 receive FIFO, which holds 16 bytes. When the FIFO is full, QEMU holds the rest back. On a board a sender with no flow control overruns the FIFO and the UART loses the bytes. So a paste of any length arrives whole in QEMU, and on a board as long as the consumer keeps up with the line rate. Under `INPUT_DROP_OLDEST` the backend reads every byte and the queue keeps the latest events, as that policy asks. When reading resumes after a wait for room, the ESC timeout starts again, so a sequence split by the wait still parses. Returns `0`, `INPUT_ERR_STATE` before `uart_init()` or on a second call, `INPUT_ERR_ARG` for another mode, or `INPUT_ERR_SPACE` when no poller or tick slot is free. Until this call the backend reads nothing, so code that calls `uart_poll()` itself keeps every byte. After it, the backend takes every byte.

| Bytes | Event |
|---|---|
| `ESC [ A`, `ESC O A` | UP |
| `ESC [ B`, `ESC O B` | DOWN |
| `ESC [ C`, `ESC O C` | RIGHT |
| `ESC [ D`, `ESC O D` | LEFT |
| CR, LF, CR LF, `ESC O M` | SELECT. LF right after CR is dropped. |
| DEL (0x7f), BS (0x08) | BACK |
| ESC alone for `INPUT_SERIAL_ESC_MS` (100 ms) | BACK |
| `ESC O P` to `ESC O S`, `ESC [ 11 ~` to `ESC [ 14 ~` | F1 to F4 |
| Tab, printable ASCII, other control codes | CHAR with the byte as `code` |
| Other sequences, Alt plus a key, bytes 0x80 and up, NUL | Nothing |

A terminal sends a key once, so each key gives a press and a release. An arrow with modifier parameters, such as `ESC [ 1 ; 5 A`, gives the plain arrow. `ESC [ 11 ; 5 ~` gives nothing. ESC followed by a control byte or another ESC counts as a lone ESC at once. A sequence still unfinished 100 ms after its ESC is dropped.

The UART keeps no receive times, so the backend stamps every byte with the time of the poll that read it. Bytes read in the same poll as the ESC still form a sequence with it. If the next poll that finds bytes starts 100 ms or more after the poll that read the ESC, the ESC becomes BACK first and the waiting bytes count on their own. So ESC, a long gap, then `[A` gives BACK, `[` and `A`. A program that stops polling for longer than the timeout can split a real sequence this way.

### Script backend

```c
#define INPUT_SCRIPT_LINE_MAX 128
int input_script_start(void);
```

Reads lines from the mini UART from `input_poll()` and answers each on the mini UART with `ok` or `err <reason>`, followed by CR LF. Returns `0`, `INPUT_ERR_STATE` on a second call, `INPUT_ERR_NO_UART` before a successful `uart_mini_init()`, or `INPUT_ERR_SPACE` when no poller slot is free. After an error you can call it again. Lines end at CR or LF and hold up to 127 characters. The backend first cuts the leading and trailing blanks, spaces and tabs, of each line, so built-in and project commands see the same words. A blank line gets no answer.

| Line | Events | Answer |
|---|---|---|
| `key <name>` | Press and release. `<name>` is an `input_key_name()`. | `ok` |
| `key <name> 1` | Press only | `ok` |
| `key <name> 0` | Release only | `ok` |
| `dial <n>` | One DIAL event on dial 0 with value n. n is signed, not 0, at most 1000000 either way. | `ok` |
| `char <c>` | One CHAR event | `ok` |
| `text <string>` | One CHAR event per character. An empty string gives none. | `ok` |
| `wait <ms>` | Holds the next lines for at least `<ms>`, unsigned decimal | `ok` when the time passed |
| `stats` | None | `ok pushed=<n> dropped=<n>` |

Words are separated by runs of blanks. For `text`, the string is the rest of the line after the blanks that follow `text`, so a run of blanks inside it stays. When the queue drops an event of the line, the answer is `err full` under `INPUT_DROP_NEWEST`. Under `INPUT_DROP_OLDEST` every event of the line was queued but older ones were dropped, and the answer is `ok dropped`.

| Answer | Cause |
|---|---|
| `err unknown command` | First word is not a command and no handler is set |
| `err unknown key` | `key` with a name `input_key_name()` does not return |
| `err bad value` | `key` with a third word other than `1` or `0` |
| `err bad steps` | `dial` with a value that is not a number, is 0 or is out of range |
| `err one character` | `char` with zero or more than one character |
| `err bad time` | `wait` with a value that is not an unsigned number |
| `err line too long` | Line over 127 characters. The backend drops the line and answers once, at its CR or LF. The next line reads normally. |

#### Project commands

```c
#define INPUT_SCRIPT_ARGS_MAX 8

typedef struct input_script_reply {
    void (*write)(const char *text);
    const char *reason;
} input_script_reply_t;

typedef int (*input_script_handler_t)(int argc, char *argv[], input_script_reply_t *reply,
                                      void *ctx);

void input_script_set_handler(input_script_handler_t fn, void *ctx);
```

A project adds its own test and maintenance commands, such as a screen dump or a state report, without taking keys from the console. `input_script_set_handler()` registers one handler. Each line whose first word is not a built-in command goes to it, split into words at runs of blanks: `argv[0]` is the command, `argc` is at most `INPUT_SCRIPT_ARGS_MAX`. More words get `err too many words` and the handler is not called. `NULL` removes the handler. You can call it before or after `input_script_start()`.

The handler answers in two parts:

| Part | How |
|---|---|
| Output lines | `reply->write(text)`. Each line reaches the host with `\| ` in front, so a dump line never looks like a status line. A line ends at `\n`. The backend ends an unfinished last line. |
| Status line | Return 0 for `ok`. Return a negative value for `err <reason>`, with `reply->reason` set first. Without a reason the answer is `err failed`. |

The handler runs in the context that polls the backend: `input_poll()` or `input_wait()` in the main loop, never an interrupt. No other script line is read while it runs, and the console and GPIO backends in tick mode keep running. It may push events and call the other input functions, but not `input_poll()` or `input_wait()`. A handler that never returns, such as one that triggers a panic, is fine: the status line never comes.

```c
static int command(int argc, char *argv[], input_script_reply_t *reply, void *ctx)
{
    if (k_strcmp(argv[0], "board") != 0 || argc != 1) {
        reply->reason = "unknown command";
        return -1;
    }
    reply->write("soc=bcm2835\n");
    return 0;
}

input_script_set_handler(command, NULL);
```

In a test, `vm.script("board")` sends the line and returns `ScriptReply(status, lines)`, here `("ok", ["soc=bcm2835"])`. See [tests/README.md](../../../tests/README.md#writing-a-target-test). [examples/input](../../../examples/input/main.c) has a `board` command.

### Pure modules

These are tested on the host. The drivers above use them. A project can use them directly, for example to decode a dial on an I/O expander.

```c
void input_queue_init(input_queue_t *q);
int input_queue_push(input_queue_t *q, const input_event_t *ev);
int input_queue_pop(input_queue_t *q, input_event_t *ev);
unsigned input_queue_count(const input_queue_t *q);
```

A ring of `INPUT_QUEUE_LEN` events with free-running indices and `pushed` and `dropped` counters. Push returns `0` or `INPUT_ERR_FULL` and drops the new event. Pop returns `1` or `0`. No locking: the caller masks IRQs.

```c
int input_queue_push_oldest(input_queue_t *q, const input_event_t *ev);
```

Like `input_queue_push()`, but a full queue drops its oldest event first and counts it in `dropped`. Always appends. Returns `0` or `INPUT_DROPPED_OLDEST`.

```c
void input_core_init(input_core_t *c);
int input_core_push(input_core_t *c, const input_event_t *ev, int *pressed);
int32_t input_core_dial_total(const input_core_t *c, unsigned dial);
int input_core_room(const input_core_t *c, unsigned n);
```

The state behind `input_push()`: a queue, the held keys, the dial totals and the overflow policy in `c->policy`. `input_core_init()` empties it all and sets `INPUT_DROP_NEWEST`. `input_core_push()` updates the held keys and the dial totals, then queues the event under the policy, and returns what `input_push()` returns. It stores the result of `input_hold_update()` in `*pressed` when `pressed` is not 0. `input_core_dial_total()` reads a total as `input_dial_total()` does. `input_core_room()` returns non-zero when `n` more events fit with no loss: always under `INPUT_DROP_OLDEST`, and when `n` slots are free under `INPUT_DROP_NEWEST`. No locking: `src/drv/input.c` masks IRQs around it.

```c
void input_debounce_init(input_debounce_t *d, unsigned stable_samples, uint32_t active_low);
uint32_t input_debounce_feed(input_debounce_t *d, uint32_t levels,
                             uint32_t *pressed, uint32_t *released);
unsigned input_debounce_samples(unsigned ms, unsigned hz);
```

A stable-count filter for up to `INPUT_DEBOUNCE_LINES` (32) lines, bit n for line n. A line changes state after `stable_samples` consecutive samples at the new level. Any sample at the old level restarts the count, so a shorter glitch never shows. `stable_samples` is clamped to 1 to 255. Every line starts released. `input_debounce_feed()` returns the debounced pressed state and stores the lines that became pressed and released at this sample. `input_debounce_samples()` converts a time to samples, rounded up, 1 to 255. It returns 1 for `hz` 0.

```c
void input_quad_init(input_quad_t *q, unsigned per_detent, int reverse, int a, int b);
int input_quad_feed(input_quad_t *q, int a, int b);
```

A Gray-code state table. A one-bit change of A/B adds or removes a transition. Transitions add up in an accumulator, and `per_detent` of them in one direction give one step. A `per_detent` other than 1 or 2 becomes 4. A reversal mid-detent cancels the partial transitions. `input_quad_feed()` returns the signed steps completed by this sample: -1, 0 or 1 with 2 or 4 per detent, and -7 to 7 with 1 per detent.

The rules for missed states:

- The state at init is the rest state, a detent. With 2 transitions per detent its complement is a rest state too. With 1 per detent every state is a detent.
- A two-bit change means one state was missed. It counts in `q->invalid` and as two transitions in the last direction. Before the first one-bit change it counts nothing, except in the waiting cases below.
- Some two-bit changes wait. The next one-bit change gives their direction, and the decoder counts the waiting transitions with it. With 4 per detent, a two-bit change that leaves a rest state waits, so a turn back that starts with a missed state still counts. A second two-bit change instead takes the last direction. With 1 per detent, a two-bit change before the first one-bit change waits, and so does each one after it until a one-bit change comes, up to 3.
- With 2 or 4 per detent, on a rest state the decoder rounds the accumulator to the nearest detent, at most one step, and starts again from 0.
- With 1 per detent the accumulator never holds a partial detent: each sample returns all its transitions as steps and clears it.

| `per_detent` | A two-bit change gives | Its direction | Cost of one missed state |
|---|---|---|---|
| 1 | 2 steps at once | The last direction, or the next one-bit change before any | None while the dial keeps its direction. 4 steps when it hides a turn back. |
| 2 | 2 transitions, one detent from a rest state | The last direction | None while the dial keeps its direction, after the first one-bit change. 2 steps when it hides a turn back. |
| 4 | 2 transitions | The next one-bit change when it leaves a rest state, else the last direction | None while the dial keeps its direction. 1 step when it hides a turn back. |

So one missed state costs nothing while the dial keeps its direction. A missed state can also hide a turn back, and no rule can tell those two paths apart from the samples. With 4 per detent that costs one step at most, with 2 per detent two, and with 1 per detent four, since every transition is a step. With 2 or 4 per detent the rounding at a rest state means a missed state never shifts the phase.

The dial must rest at a detent at init. If it does not, its state at init still becomes the rest state, and nothing heals that: each detent still gives one step, but at the wrong point of the turn, so the total is off by one at most, in one direction of turn. A dial turned back past its start reads right again. The host test `quad_start_off_a_detent` shows this.

```c
void input_ansi_init(input_ansi_t *p, uint32_t esc_timeout_ms);
int input_ansi_feed(input_ansi_t *p, uint8_t byte, uint32_t now_ms, input_event_t *out);
int input_ansi_poll(input_ansi_t *p, uint32_t now_ms, input_event_t *out);
void input_ansi_resume(input_ansi_t *p, uint32_t now_ms);
```

The parser behind the console backend. `input_ansi_feed()` writes up to `INPUT_ANSI_OUT_MAX` (2) events and returns their count. Keys come out as presses only, with `source` and `t_ms` 0. Call `input_ansi_poll()` when no byte came. It turns a lone ESC into BACK once `esc_timeout_ms` passed since the ESC, and drops an unfinished sequence after the same time. It returns 0 or 1. `input_ansi_feed()` makes the same check before it takes the byte, so a byte fed after the timeout never joins the old sequence. Sequences may arrive split across calls within the timeout. `input_ansi_resume()` restarts the timeout of a pending ESC or sequence at `now_ms`. A reader that left bytes waiting calls it when it reads again. It does nothing in the ground state.

```c
void input_hold_init(input_hold_t *h);
int input_hold_update(input_hold_t *h, const input_event_t *ev);
int input_hold_down(const input_hold_t *h, unsigned code);
```

The held-key state behind `input_key_down()` and auto-repeat. It keeps one bit per key code below 32 for each of the sources 0 to 3 (`INPUT_HOLD_SOURCES`). `input_hold_init()` clears the held keys and the repeat key. `input_hold_update()` applies a KEY event and ignores other events, codes above 31 and sources above 3. A press marks the key held by its source, makes it the repeat key and returns `1`, so the caller restarts the repeat delay. A release clears only that source's hold and returns `0`. If another source still holds the repeat key, the repeat goes on from that source. A release from a source that did not hold the key changes nothing. `input_hold_down()` returns non-zero while any source holds `code`. No locking: `src/drv/input.c` masks IRQs around it.

```c
void input_line_init(input_line_t *l);
int input_line_feed(input_line_t *l, char c);
char *input_line_trim(char *s);
char *input_line_word(char *s);
int input_line_split(char *s, char *argv[], int max);
```

The line reader and tokenizer behind the script backend. Blanks are spaces and tabs. `input_line_feed()` takes one byte. CR or LF ends a line: a line with words gives `INPUT_LINE_READY` and `l->line` holds it, trimmed, until the next call. A blank line gives `INPUT_LINE_NONE`, like any other byte. A line over `INPUT_SCRIPT_LINE_MAX - 1` bytes gives `INPUT_LINE_LONG` once, at its end, and the next byte starts a new line. `input_line_trim()` cuts leading blanks and trailing blanks and CR in place and returns the new start. `input_line_word()` ends the first word in place and returns what follows its blanks. `input_line_split()` splits at runs of blanks into `argv`, which holds `max + 1` entries, ends it with `NULL` and returns the word count, `0` for a blank string or `-1` for more than `max` words.

### Context rules

| Function | Context |
|---|---|
| `input_push()`, `input_pending()`, `input_flush()`, `input_stats()`, `input_key_down()`, `input_dial_total()`, `input_set_overflow()`, `input_dropped()`, `input_room()` | Any |
| `input_poll()`, `input_wait()` | Main only. They run the main-loop backends. |
| `input_init()`, `input_set_repeat()`, `input_gpio_*()` setup, `input_serial_start()`, `input_script_start()`, `input_script_set_handler()`, `input_add_poller()` | Main only |
| GPIO sampling, repeat, console in tick mode | IRQ, from tick callbacks |

### Sharp edges

| Edge | Detail |
|---|---|
| One consumer. | Only one loop should call `input_poll()` or `input_wait()`. |
| Drops keep the newest out by default. | A full queue refuses the new event unless `input_set_overflow(INPUT_DROP_OLDEST)` is set. Under either policy a dropped release still clears the key state and a dropped DIAL event still counts in `input_dial_total()`, but the consumer never sees the event. |
| The guard is final. | A `disabled_stuck` backend never restarts. If one button is held at power-on and the others are wired, the guard passes. |
| The dial is sampled, not interrupt driven. | It is read on the tick, 100 Hz in `examples/input`. Each state of the turn must last longer than the gap between two samples, or at most one state may hide in a gap. See [Dial speed](#dial-speed). |
| The dial starts at a detent. | Its state at init is taken as a detent. A dial between detents at init keeps an offset of up to one step. |
| Repeat follows the last press. | Pressing a second key moves the repeat to it. Releasing the second key stops repeat even if the first is still held. Releasing a key on one source keeps the repeat while another source holds it. |
| Key state covers codes 0 to 31. | `input_key_down()` and repeat ignore KEY events with a higher code. |
| The console backend takes every byte. | After `input_serial_start()`, `uart_getc()` and `uart_poll()` in other code compete for bytes. |
| Serial input needs a busy loop. | In main mode the backends read only inside `input_poll()`. A main loop that stalls longer than the UART FIFO lasts loses bytes on a board. The PL011 FIFO holds 16 bytes and the mini UART FIFO 8. A full queue under `INPUT_DROP_NEWEST` stalls reading the same way: the console backend leaves bytes in the FIFO instead of dropping them. |
| `wait` blocks the script, not the system. | Lines after `wait` stay in the mini UART FIFO and the host until the time passes. On a board the mini UART receive FIFO holds 8 bytes, so a host must not send more during a `wait`. See [Mini UART](#mini-uart-uart_minih). |
| Timestamps use the system timer. | `t_ms` is `timer_now_ms()`, not `tick_ms()`, so it is valid from IRQ context and before the tick starts. |

### Example

From [`examples/input/main.c`](../../../examples/input/main.c), shortened:

```c
uart_init(115200);
mini = uart_mini_init(115200);
irq_init();
tick_init(TICK_HZ);
irq_cpu_enable();

input_init();
for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
    input_gpio_add_button(buttons[i].pin, buttons[i].code, 0, GPIO_PULL_UP);
}
input_gpio_add_dial(INPUT_PIN_DIAL_A, INPUT_PIN_DIAL_B, INPUT_PIN_DIAL_PUSH, 4);
gpio = input_gpio_start(TICK_HZ, DEBOUNCE_MS);
input_serial_start(INPUT_SERIAL_MAIN);
input_script_start();

for (;;) {
    if (!input_wait(&ev, 1000)) {
        continue;
    }
    print_event(&ev);
}
```

`TICK_HZ` is 100 and `DEBOUNCE_MS` is 20. The `repeat` variant (`-DINPUT_DEMO_REPEAT=1`) also calls `input_set_repeat(300, 100)`.

### Wiring notes

`examples/input/robco_config.h` uses these BCM pins. Each button connects its pin to ground and uses the internal pull-up.

| Function | Macro | Pin |
|---|---|---|
| UP | `INPUT_PIN_UP` | GPIO5 |
| DOWN | `INPUT_PIN_DOWN` | GPIO6 |
| LEFT | `INPUT_PIN_LEFT` | GPIO17 |
| RIGHT | `INPUT_PIN_RIGHT` | GPIO22 |
| SELECT | `INPUT_PIN_SELECT` | GPIO26 |
| BACK | `INPUT_PIN_BACK` | GPIO27 |
| Dial A | `INPUT_PIN_DIAL_A` | GPIO23 |
| Dial B | `INPUT_PIN_DIAL_B` | GPIO24 |
| Dial push | `INPUT_PIN_DIAL_PUSH` | GPIO16 |

They avoid GPIO0 and 1 (ID EEPROM), 2 and 3 (I2C1), 7 to 11 (SPI0), 12 and 13 (PWM audio on some boards), 14 and 15 (UART), 18 to 21 (PCM) and 34 to 53 (SD card, Wi-Fi and audio).

A mechanical encoder usually has A, B and a common pin: wire common to ground. Most give 4 transitions per detent. Some give 2 or 1. On BCM2711 `gpio_set_pull()` fails, so add external pull-ups there.

### Testing with real lines

QEMU's GPIO model takes a level from outside: a qtest write to GPSET0/1 or GPCLR0/1 sets or clears that pin's GPLEV bit, input pins included (V19). `tests/harness/pigpio.py` does this. Pulls are not modelled, so an undriven line reads 0. Drive every active-low line high before the guest starts, or the stuck-line guard turns the backend off:

```python
with boot(img, board, qtest=True, gpio_high=[5, 6, 16, 23, 24]) as vm:
    io = PiGpio(vm, board, names={"select": 5})
    io.tap("select", 0.15)
    io.turn(23, 24, detents=3, state_s=0.03)
```

`tests/target/test_input_gpio.py` runs the buttons, repeat, the guard, the dial and its limit this way on all four boards. Timing comes from the host, so each test names its allowance.

The QEMU tick is not exact. Gaps between callbacks are often 12 to 13 ms and reach 55 ms under host load. Each timed run reads the guest's `ticks` command before and after, so its numbers cover that run only. The missed count never resets, so the test subtracts the count read before the run. A run is tried again, then skipped with its numbers, only when they show the guest could not see what the test claims. Any other wrong result fails.

| Check | It counts the run when |
|---|---|
| A glitch or a chatter burst gives nothing | No tick was missed and the burst, from its first press to the real end of its last, is shorter than 10 ms less the tick jitter |
| A tap gives one press and one release | No tick gap reached half the hold |
| Repeats stay between 6 and 8 | The real hold, give or take three tick gaps, stays between 800 and 1100 ms |
| A slow dial turn counts each detent | No tick gap reached the shortest state, less the time the host cut and the bounce |

### Pending hardware validation

QEMU proves the logic with levels driven from outside, not real contacts, pulls or edges. See [HARDWARE_VALIDATION.md](../../HARDWARE_VALIDATION.md).

| Item | Equipment | What to look for |
|---|---|---|
| Buttons and dial | Buttons and encoder, serial adapter | `examples/input` reports `gpio status=active`, one press and release per click, one DIAL event per detent with the right sign. |
| Mini UART script channel | Serial adapter on GPIO14 and 15 set to ALT5 | `ready` and `ok` answers at 115200 baud with `core_freq` fixed. |

## Mini UART (`uart_mini.h`)

`uart_mini.h` drives the mini UART in the AUX block at `periph_base + 0x215000`, 8N1, polled. It is a test and bring-up channel. In QEMU it is the second `-serial`.

| Constant | Value | Meaning |
|---|---|---|
| `UART_MINI_FALLBACK_CORE_CLOCK` | `250000000` | Core clock used when the mailbox gives none |
| `MBOX_CLOCK_CORE` | `4` | Mailbox clock id of the VPU core clock |
| `UART_MINI_ERR_BOARD` | `-1` | No peripheral base |
| `UART_MINI_ERR_BAUD` | `-2` | Baud 0 or out of range for the core clock |

```c
int uart_mini_init(uint32_t baud);
```

Sets the enable bit in AUXENB, 8-bit mode, no interrupts and no flow control, clears both FIFOs, and sets the baud register from the core clock. The clock comes from the mailbox (`MBOX_CLOCK_CORE`). Without a mailbox answer it assumes `UART_MINI_FALLBACK_CORE_CLOCK`. The register follows baud = core clock / (8 * (register + 1)), rounded to the nearest value. Returns `0`, `UART_MINI_ERR_BOARD`, or `UART_MINI_ERR_BAUD` when `baud` is 0, above core clock / 8, or needs a register above 0xffff.

```c
void uart_mini_putc(char c);        /* '\n' goes out as "\r\n" */
int uart_mini_getc(void);           /* blocking; -1 at once before uart_mini_init() */
int uart_mini_poll(void);           /* -1 when nothing is pending */
int uart_mini_ready(void);          /* 1 after uart_mini_init() succeeded, else 0 */
void uart_mini_puts(const char *s);
```

These behave like their `uart_*` counterparts. Before init, `uart_mini_putc()` and `uart_mini_puts()` do nothing, and `uart_mini_poll()` and `uart_mini_getc()` return -1 at once. After init, `uart_mini_getc()` waits for a byte. `uart_mini_ready()` tells whether init succeeded.

`uart_mini_init()` does not mux any pin. In QEMU it works without pins. On a board its pins are GPIO14 and 15 (ALT5), the same pins as the PL011 console, and a board can only wire one of them there. To use the script channel on a board, a project must either give up the PL011 on those pins, or route the PL011 elsewhere and set GPIO14 and 15 to ALT5 itself before `uart_mini_init()`. The mini UART baud also follows the core clock, which the firmware can change, so fix `core_freq` in `config.txt` when you use it on a board.

| Edge | Detail |
|---|---|
| The receive FIFO holds 8 bytes. | Nothing reads the mini UART in the background: the script backend reads it only inside `input_poll()`, and holds the bytes during a `wait`. On a board, a host that sends more than 8 bytes while the consumer is busy, during a `wait` or a long frame, loses the rest. QEMU does not show this. A project should pace the host: send a line, then wait for its `ok` before the next one. If it cannot, it should call `input_poll()` at least every 8 byte times, about 0.7 ms at 115200 baud, or lower the baud rate. |
| The baud drifts with the core clock. | The register is set once from the clock at init. A later core clock change breaks the baud. |
