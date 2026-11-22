## Input events (`input.h`)

`input.h` turns GPIO buttons, a rotary dial and serial lines into one stream of events. Backends push events into a fixed queue from IRQ or main context. The main loop reads them with `input_poll()` or `input_wait()`. Serial input lets a person or a test drive a product in QEMU, where GPIO inputs cannot change.

| Part | File | Touches hardware |
|---|---|---|
| Ring queue | `src/lib/input_queue.c` | No |
| Debouncer | `src/lib/input_debounce.c` | No |
| Quadrature decoder | `src/lib/input_quad.c` | No |
| ANSI/VT parser | `src/lib/input_ansi.c` | No |
| Queue, key state, repeat | `src/drv/input.c` | IRQ mask, system timer |
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
| `INPUT_KEY_UP` | `1` | | `INPUT_KEY_F1` | `7` |
| `INPUT_KEY_DOWN` | `2` | | `INPUT_KEY_F2` | `8` |
| `INPUT_KEY_LEFT` | `3` | | `INPUT_KEY_F3` | `9` |
| `INPUT_KEY_RIGHT` | `4` | | `INPUT_KEY_F4` | `10` |
| `INPUT_KEY_SELECT` | `5` | | `INPUT_KEY_DIAL_PUSH` | `11` |
| `INPUT_KEY_BACK` | `6` | | `INPUT_KEY_COUNT` | `12` |

| Error | Value | Meaning |
|---|---|---|
| `INPUT_ERR_FULL` | `-1` | Queue full. The event is dropped and counted. |
| `INPUT_ERR_ARG` | `-2` | Bad argument |
| `INPUT_ERR_NO_IRQ` | `-3` | `irq_init()` has not run |
| `INPUT_ERR_TICK` | `-4` | The tick does not run at the given rate |
| `INPUT_ERR_SPACE` | `-5` | A fixed table or the tick callback slots are full |
| `INPUT_ERR_STATE` | `-6` | Wrong call order, or already started |

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

The GPIO backend, auto-repeat and the console backend in tick mode each take one of the four tick callback slots.

### Queue and key state

```c
int input_init(void);
```

Empties the queue and clears the counters, the key state and the repeat timer. Returns `0`. Call it once before any backend starts.

```c
int input_push(const input_event_t *ev);
```

Copies the event into the queue and stamps `t_ms` with `timer_now_ms()`. For a KEY event it also updates the key state and the repeat timer. Returns `0`, or `INPUT_ERR_FULL` when the queue holds `INPUT_QUEUE_LEN` (32) events: the new event is dropped and counted, the queued ones stay. Safe from any context. Backends call it; a project can call it too, for example to inject events from its own driver.

```c
int input_poll(input_event_t *ev);
```

Runs the main-loop backends (the console backend in `INPUT_SERIAL_MAIN` mode and the script backend), then takes the oldest event. Returns `1` with the event in `*ev`, or `0`. Never blocks. Main context only.

```c
#define INPUT_WAIT_FOREVER  0xffffffffu
int input_wait(input_event_t *ev, uint32_t timeout_ms);
```

Calls `input_poll()` until an event comes or `timeout_ms` passes. Returns `1` or `0`. Between checks it sleeps in `wfi` when CPU IRQs are on and the tick runs, so a serial byte waits at most one tick period. With IRQs masked or no tick it spins. Main context only.

```c
unsigned input_pending(void);
void input_flush(void);
void input_stats(uint32_t *pushed, uint32_t *dropped);
```

`input_pending()` counts queued events. `input_flush()` drops them without changing the counters. `input_stats()` stores the events accepted and dropped since `input_init()`; either pointer may be 0.

```c
int input_key_down(unsigned code);
```

Non-zero while `code` is pressed, from any source. The state follows the KEY events pushed, even those the queue dropped.

```c
int input_set_repeat(uint32_t delay_ms, uint32_t rate_ms);
```

Turns on auto-repeat: while a key is held, the first `INPUT_REPEAT` event comes `delay_ms` after the press, then one every `rate_ms`. Only the last key pressed repeats. `delay_ms` 0 turns it off, which is the default. It runs from a tick callback, registered on the first call. Returns `0`, `INPUT_ERR_ARG` for a zero rate, or `INPUT_ERR_SPACE` when no tick slot is free. After a stall it goes on from the current time instead of sending a burst.

```c
int input_add_poller(void (*fn)(void));
const char *input_key_name(unsigned code);
```

`input_add_poller()` adds a function `input_poll()` runs in main context, at most four. Backends use it. `input_key_name()` returns `"up"`, `"f1"`, `"dial_push"` and so on, or 0 for an unknown code.

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

Registers a button. `active_level` is `0` when the pressed button reads low (a button to ground with `GPIO_PULL_UP`) or `1`. `pull` is a `GPIO_PULL_*` code. Returns `0`, `INPUT_ERR_ARG`, `INPUT_ERR_SPACE`, or `INPUT_ERR_STATE` after `input_gpio_start()`.

```c
int input_gpio_add_dial(unsigned pin_a, unsigned pin_b, int push_pin, unsigned per_detent);
```

Registers a quadrature dial with pull-ups on both pins, and an optional active-low push button that reports `INPUT_KEY_DIAL_PUSH`. `per_detent` is the number of A/B transitions per click: 1, 2 or 4. Swap `pin_a` and `pin_b` if the dial counts the wrong way. Returns like `input_gpio_add_button()`.

```c
int input_gpio_start(uint32_t tick_hz, unsigned debounce_ms);
```

Sets the pins to input with their pulls, then samples every pin from a tick callback, once per tick. Buttons go through the debouncer with a stable time of `debounce_ms`, rounded up to whole ticks. Dials go through the quadrature decoder, one DIAL event per detent. Returns `0`, `INPUT_ERR_NO_IRQ` before `irq_init()`, `INPUT_ERR_TICK` when `tick_init(tick_hz)` does not return `0`, `INPUT_ERR_SPACE` when no tick slot is free, or `INPUT_ERR_STATE` on a second call. Main context.

`tick.h` cannot report the running rate, so `input_gpio_start()` calls `tick_init(tick_hz)`. If the tick already runs at that rate, this returns `0` and changes nothing. If it does not run yet, the call starts it.

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

The stuck-line guard samples for the first 100 ms of ticks and pushes no event. If every registered button reads active for all of it, the lines are taken as not wired, as in QEMU where every input reads 0 and active-low buttons look pressed. The backend then stops for good. Otherwise it turns active and reports a press for each button already held. With no button registered, the guard always passes.

### Console backend

```c
#define INPUT_SERIAL_MAIN   0
#define INPUT_SERIAL_TICK   1
int input_serial_start(int mode);
```

Reads the PL011 console through the ANSI parser. `INPUT_SERIAL_MAIN` reads from `input_poll()`; `INPUT_SERIAL_TICK` reads from a tick callback, up to 64 bytes per tick. Returns `0`, `INPUT_ERR_STATE` before `uart_init()` or on a second call, `INPUT_ERR_ARG`, or `INPUT_ERR_SPACE`. Until this call the backend reads nothing, so code that calls `uart_poll()` itself keeps every byte. After it, the backend takes every byte.

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

A terminal sends a key once, so each key gives a press and a release. Modifier parameters such as `ESC [ 1 ; 5 A` are ignored. ESC followed by a control byte or another ESC counts as a lone ESC at once.

### Script backend and the mini UART

```c
int input_script_start(void);
```

Reads lines from the mini UART from `input_poll()` and answers each with `ok` or `err <reason>`, followed by CR LF. Returns `0`, `INPUT_ERR_STATE` on a second call, or `INPUT_ERR_SPACE`. Lines end at CR or LF and hold up to 127 characters; empty lines get no answer.

| Line | Events | Answer |
|---|---|---|
| `key <name>` | Press and release. `<name>` is an `input_key_name()`. | `ok` |
| `key <name> 1` | Press only | `ok` |
| `key <name> 0` | Release only | `ok` |
| `dial <+n or -n>` | One DIAL event on dial 0 with value n, n not 0 | `ok` |
| `char <c>` | One CHAR event | `ok` |
| `text <string>` | One CHAR event per character | `ok` |
| `wait <ms>` | Holds the next lines for at least `<ms>` | `ok` when the time passed |
| `stats` | None | `ok pushed=<n> dropped=<n>` |

When the queue drops an event of the line, the answer is `err full`. Other errors: `err unknown command`, `err unknown key`, `err bad value`, `err bad steps`, `err one character`, `err bad time`, `err line too long`.

```c
int uart_mini_init(uint32_t baud);
void uart_mini_putc(char c);        /* '\n' goes out as "\r\n" */
int uart_mini_getc(void);           /* blocking */
int uart_mini_poll(void);           /* -1 when nothing is pending */
void uart_mini_puts(const char *s);
```

`uart_mini.h` drives the mini UART in the AUX block at `periph_base + 0x215000`. `uart_mini_init()` sets the enable bit in AUXENB, 8-bit mode, no interrupts, and the baud register from the core clock (mailbox clock id 4, `MBOX_CLOCK_CORE`), with baud = core clock / (8 * (register + 1)). Without a mailbox answer it assumes `UART_MINI_FALLBACK_CORE_CLOCK` (250 MHz). Returns `0`, `UART_MINI_ERR_BOARD` or `UART_MINI_ERR_BAUD`. The other functions behave like their `uart_*` counterparts and do nothing before init.

`uart_mini_init()` does not mux any pin. In QEMU the second `-serial` is the mini UART, so it works without pins. On a board it is a bring-up channel only. See the wiring notes.

### Pure modules

These are tested on the host. The drivers above use them; a project can use them directly, for example to decode a dial on an I/O expander.

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

A stable-count filter for up to 32 lines, bit n for line n. A line changes state after `stable_samples` consecutive samples at the new level; any sample at the old level restarts the count, so a shorter glitch never shows. Every line starts released. `input_debounce_feed()` returns the debounced pressed state and stores the lines that became pressed and released at this sample. `input_debounce_samples()` converts a time to samples, rounded up, 1 to 255.

```c
void input_quad_init(input_quad_t *q, unsigned per_detent, int reverse, int a, int b);
int input_quad_feed(input_quad_t *q, int a, int b);
```

A Gray-code state table. A one-bit change of A/B adds or removes a transition; a two-bit change means a sample was missed, so it is rejected and counted in `q->invalid`. Transitions add up in an accumulator, and each `per_detent` of them in one direction gives one step. `input_quad_feed()` returns -1, 0 or 1. A reversal mid-detent cancels the partial transitions.

```c
void input_ansi_init(input_ansi_t *p, uint32_t esc_timeout_ms);
int input_ansi_feed(input_ansi_t *p, uint8_t byte, uint32_t now_ms, input_event_t *out);
int input_ansi_poll(input_ansi_t *p, uint32_t now_ms, input_event_t *out);
```

The parser behind the console backend. `input_ansi_feed()` writes up to `INPUT_ANSI_OUT_MAX` (2) events and returns their count. Keys come out as presses only, with `source` and `t_ms` 0. Call `input_ansi_poll()` when no byte came: it turns a lone ESC into BACK once `esc_timeout_ms` passed, and drops an unfinished sequence. Sequences may arrive split across calls.

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
| Repeat follows the last press. | Pressing a second key moves the repeat to it. Releasing the second key stops repeat even if the first is still held. |
| The console backend takes every byte. | After `input_serial_start()`, `uart_getc()` and `uart_poll()` in other code compete for bytes. |
| `wait` blocks the script, not the system. | Lines after `wait` stay in the mini UART FIFO and the host until the time passes. |
| Timestamps use the system timer. | `t_ms` is `timer_now_ms()`, not `tick_ms()`, so it is valid from IRQ context and before the tick starts. |

### Example

From `examples/input/main.c`:

```c
irq_init();
tick_init(100);
irq_cpu_enable();

input_init();
input_gpio_add_button(INPUT_PIN_UP, INPUT_KEY_UP, 0, GPIO_PULL_UP);
input_gpio_add_dial(INPUT_PIN_DIAL_A, INPUT_PIN_DIAL_B, INPUT_PIN_DIAL_PUSH, 4);
input_gpio_start(100, 20);
input_serial_start(INPUT_SERIAL_MAIN);
input_script_start();

for (;;) {
    if (input_wait(&ev, 1000)) {
        print_event(&ev);
    }
}
```

### Wiring notes

`examples/input/robco_config.h` uses these BCM pins. Each button connects its pin to ground and uses the internal pull-up.

| Function | Pin | | Function | Pin |
|---|---|---|---|---|
| UP | GPIO5 | | BACK | GPIO27 |
| DOWN | GPIO6 | | Dial A | GPIO23 |
| LEFT | GPIO17 | | Dial B | GPIO24 |
| RIGHT | GPIO22 | | Dial push | GPIO16 |
| SELECT | GPIO26 | | | |

They avoid GPIO0 and 1 (ID EEPROM), 2 and 3 (I2C1), 7 to 11 (SPI0), 12 and 13 (PWM audio on some boards), 14 and 15 (UART), 18 to 21 (PCM) and 34 to 53 (SD card, Wi-Fi and audio).

A mechanical encoder usually has A, B and a common pin: wire common to ground. Most give 4 transitions per detent; some give 2 or 1. On BCM2711 `gpio_set_pull()` fails, so add external pull-ups there.

The mini UART uses GPIO14 and 15 (ALT5), the same pins as the PL011 console. A board can only wire one of them there. To use the script channel on a board, a project must either give up the PL011 on those pins, or route the PL011 elsewhere and set GPIO14 and 15 to ALT5 itself before `uart_mini_init()`. The mini UART baud also follows the core clock, which the firmware can change, so fix `core_freq` in `config.txt` when it is used on a board.
