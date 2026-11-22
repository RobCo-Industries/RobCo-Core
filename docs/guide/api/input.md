# Input

This page covers the input event queue with its GPIO, console and script backends, and the mini UART that carries the script channel. Rules that apply to every header are in [Conventions](../API.md#conventions).

## Input events (`input.h`)

`input.h` turns GPIO buttons, a rotary dial and serial lines into one stream of events. Backends push events into a fixed queue from IRQ or main context. The main loop reads them with `input_poll()` or `input_wait()`. Serial input lets a person or a test drive a product in QEMU, where GPIO inputs cannot change.

| Part | File | Touches hardware |
|---|---|---|
| Ring queue | `src/lib/input_queue.c` | No |
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

Empties the queue, clears the counters and the key state, and stops a repeat in progress. It keeps the repeat delay and rate and the registered pollers. Returns `0`. Call it once before any backend starts.

```c
int input_push(const input_event_t *ev);
```

Copies the event into the queue and stamps `t_ms` with `timer_now_ms()`. For a KEY event it also updates the key state and the repeat timer. Returns `0`, or `INPUT_ERR_FULL` when the queue holds `INPUT_QUEUE_LEN` (32) events: the new event is dropped and counted, the queued ones stay. Safe from any context. Backends call it. A project can call it too, for example to inject events from its own driver.

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

Sets the pins to input with their pulls, then samples every pin from a tick callback, once per tick. Buttons go through the debouncer with a stable time of `debounce_ms`, rounded up to whole ticks (1 to 255). Dials go through the quadrature decoder, one DIAL event per detent. Returns `0`, `INPUT_ERR_NO_IRQ` before `irq_init()`, `INPUT_ERR_TICK` when `tick_init(tick_hz)` does not return `0`, `INPUT_ERR_SPACE` when no tick slot is free, or `INPUT_ERR_STATE` on a second call. Main context.

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

The stuck-line guard samples for the first 100 ms of ticks (at least one tick) and pushes no event. If every registered button reads active at every sample, the lines are taken as not wired, as in QEMU where every input reads 0 and active-low buttons look pressed. The backend then stops for good. Otherwise it turns active and reports a press for each button whose debounced state is pressed. With no button registered, the guard always passes.

### Console backend

```c
#define INPUT_SERIAL_MAIN   0
#define INPUT_SERIAL_TICK   1
#define INPUT_SERIAL_ESC_MS 100
int input_serial_start(int mode);
```

Reads the PL011 console through the ANSI parser. `INPUT_SERIAL_MAIN` reads from `input_poll()`. `INPUT_SERIAL_TICK` reads from a tick callback. Each call reads up to 64 bytes. Returns `0`, `INPUT_ERR_STATE` before `uart_init()` or on a second call, `INPUT_ERR_ARG` for another mode, or `INPUT_ERR_SPACE` when no poller or tick slot is free. Until this call the backend reads nothing, so code that calls `uart_poll()` itself keeps every byte. After it, the backend takes every byte.

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

### Script backend

```c
#define INPUT_SCRIPT_LINE_MAX 128
int input_script_start(void);
```

Reads lines from the mini UART from `input_poll()` and answers each on the mini UART with `ok` or `err <reason>`, followed by CR LF. Returns `0`, `INPUT_ERR_STATE` on a second call, `INPUT_ERR_NO_UART` before a successful `uart_mini_init()`, or `INPUT_ERR_SPACE` when no poller slot is free. After an error you can call it again. Lines end at CR or LF and hold up to 127 characters. Empty lines get no answer.

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

Words are separated by one space. When the queue drops an event of the line, the answer is `err full`.

| Answer | Cause |
|---|---|
| `err unknown command` | First word is not a command |
| `err unknown key` | `key` with a name `input_key_name()` does not return |
| `err bad value` | `key` with a third word other than `1` or `0` |
| `err bad steps` | `dial` with a value that is not a number, is 0 or is out of range |
| `err one character` | `char` with zero or more than one character |
| `err bad time` | `wait` with a value that is not an unsigned number |
| `err line too long` | Line over 127 characters. The line is ignored. |

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

A Gray-code state table. A one-bit change of A/B adds or removes a transition. A two-bit change means a sample was missed, so it is rejected and counted in `q->invalid`. Transitions add up in an accumulator, and each `per_detent` of them in one direction gives one step. A `per_detent` other than 1 or 2 becomes 4. `input_quad_feed()` returns -1, 0 or 1. A reversal mid-detent cancels the partial transitions.

```c
void input_ansi_init(input_ansi_t *p, uint32_t esc_timeout_ms);
int input_ansi_feed(input_ansi_t *p, uint8_t byte, uint32_t now_ms, input_event_t *out);
int input_ansi_poll(input_ansi_t *p, uint32_t now_ms, input_event_t *out);
```

The parser behind the console backend. `input_ansi_feed()` writes up to `INPUT_ANSI_OUT_MAX` (2) events and returns their count. Keys come out as presses only, with `source` and `t_ms` 0. Call `input_ansi_poll()` when no byte came. It turns a lone ESC into BACK once `esc_timeout_ms` passed since the ESC, and drops an unfinished sequence after the same time. It returns 0 or 1. Sequences may arrive split across calls.

```c
void input_hold_init(input_hold_t *h);
int input_hold_update(input_hold_t *h, const input_event_t *ev);
int input_hold_down(const input_hold_t *h, unsigned code);
```

The held-key state behind `input_key_down()` and auto-repeat. It keeps one bit per key code below 32 for each of the sources 0 to 3 (`INPUT_HOLD_SOURCES`). `input_hold_init()` clears the held keys and the repeat key. `input_hold_update()` applies a KEY event and ignores other events, codes above 31 and sources above 3. A press marks the key held by its source, makes it the repeat key and returns `1`, so the caller restarts the repeat delay. A release clears only that source's hold and returns `0`. If another source still holds the repeat key, the repeat goes on from that source. A release from a source that did not hold the key changes nothing. `input_hold_down()` returns non-zero while any source holds `code`. No locking: `src/drv/input.c` masks IRQs around it.

### Context rules

| Function | Context |
|---|---|
| `input_push()`, `input_pending()`, `input_flush()`, `input_stats()`, `input_key_down()` | Any |
| `input_poll()`, `input_wait()` | Main only. They run the main-loop backends. |
| `input_init()`, `input_set_repeat()`, `input_gpio_*()` setup, `input_serial_start()`, `input_script_start()`, `input_add_poller()` | Main only |
| GPIO sampling, repeat, console in tick mode | IRQ, from tick callbacks |

### Sharp edges

| Edge | Detail |
|---|---|
| One consumer. | Only one loop should call `input_poll()` or `input_wait()`. |
| Drops keep the newest out. | A full queue refuses the new event. A dropped release still clears the key state, but the consumer never sees it. |
| The guard is final. | A `disabled_stuck` backend never restarts. If one button is held at power-on and the others are wired, the guard passes. |
| No resync on the dial. | A missed sample that looks legal shifts the detent phase by one transition until power-off. |
| Repeat follows the last press. | Pressing a second key moves the repeat to it. Releasing the second key stops repeat even if the first is still held. Releasing a key on one source keeps the repeat while another source holds it. |
| Key state covers codes 0 to 31. | `input_key_down()` and repeat ignore KEY events with a higher code. |
| The console backend takes every byte. | After `input_serial_start()`, `uart_getc()` and `uart_poll()` in other code compete for bytes. |
| Serial input needs a busy loop. | In main mode the backends read only inside `input_poll()`. A main loop that stalls longer than the UART FIFO lasts loses bytes. |
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

### Pending hardware validation

QEMU cannot change GPIO input levels, so it proves only the stuck-line guard. See [HARDWARE_VALIDATION.md](../../HARDWARE_VALIDATION.md).

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
