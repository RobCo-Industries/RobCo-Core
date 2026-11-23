#ifndef ROBCO_INPUT_H
#define ROBCO_INPUT_H

#include <stdint.h>

/*
 * Input events from GPIO buttons, a rotary dial and serial lines.
 * Backends push events into one queue. The main loop reads them.
 */

/* Event types. */
#define INPUT_EV_KEY        1   /* value: 1 press, 0 release, 2 repeat */
#define INPUT_EV_DIAL       2   /* code: dial index, value: signed detent steps */
#define INPUT_EV_CHAR       3   /* code: character or control code from a serial line */

/* Key values. */
#define INPUT_RELEASE       0
#define INPUT_PRESS         1
#define INPUT_REPEAT        2

/* Sources. */
#define INPUT_SRC_GPIO      0
#define INPUT_SRC_SERIAL    1
#define INPUT_SRC_SCRIPT    2

/* Logical key codes. 0 is no key. */
#define INPUT_KEY_NONE      0
#define INPUT_KEY_UP        1
#define INPUT_KEY_DOWN      2
#define INPUT_KEY_LEFT      3
#define INPUT_KEY_RIGHT     4
#define INPUT_KEY_SELECT    5
#define INPUT_KEY_BACK      6
#define INPUT_KEY_F1        7
#define INPUT_KEY_F2        8
#define INPUT_KEY_F3        9
#define INPUT_KEY_F4        10
#define INPUT_KEY_DIAL_PUSH 11
#define INPUT_KEY_COUNT     12

typedef struct {
    uint8_t type;       /* INPUT_EV_* */
    uint8_t source;     /* INPUT_SRC_* */
    uint16_t code;      /* INPUT_KEY_*, dial index or character */
    int32_t value;
    uint32_t t_ms;      /* timer_now_ms() when pushed */
} input_event_t;

/* Errors. */
#define INPUT_ERR_FULL      (-1)    /* queue full, event dropped and counted */
#define INPUT_ERR_ARG       (-2)    /* bad argument */
#define INPUT_ERR_NO_IRQ    (-3)    /* irq_init() has not run */
#define INPUT_ERR_TICK      (-4)    /* the tick does not run at the given rate */
#define INPUT_ERR_SPACE     (-5)    /* a fixed table is full */
#define INPUT_ERR_STATE     (-6)    /* wrong call order */
#define INPUT_ERR_NO_UART   (-7)    /* the UART the backend reads is not initialised */

/* input_push() under INPUT_DROP_OLDEST: the event was queued, the oldest one dropped. */
#define INPUT_DROPPED_OLDEST 1

/* Overflow policies, input_set_overflow(). */
#define INPUT_DROP_NEWEST   0       /* a full queue refuses the new event (default) */
#define INPUT_DROP_OLDEST   1       /* a full queue drops its oldest event for the new one */

/* Dials with a running total, input_dial_total(). Covers INPUT_GPIO_MAX_DIALS. */
#define INPUT_DIAL_COUNT    2

/* ---- Pure ring queue, src/lib/input_queue.c ---- */

#define INPUT_QUEUE_LEN     32u     /* power of two */

typedef struct {
    input_event_t buf[INPUT_QUEUE_LEN];
    uint32_t head;      /* next write, free-running */
    uint32_t tail;      /* next read, free-running */
    uint32_t pushed;    /* events accepted */
    uint32_t dropped;   /* events refused because the queue was full */
} input_queue_t;

void input_queue_init(input_queue_t *q);
/* Appends a copy. Returns 0, or INPUT_ERR_FULL and counts a drop. */
int input_queue_push(input_queue_t *q, const input_event_t *ev);
/* Removes the oldest event into *ev. Returns 1, or 0 when empty. */
int input_queue_pop(input_queue_t *q, input_event_t *ev);
unsigned input_queue_count(const input_queue_t *q);

/*
 * Like input_queue_push(), but a full queue first drops its oldest event
 * and counts it in `dropped`. Returns 0, or INPUT_DROPPED_OLDEST.
 */
int input_queue_push_oldest(input_queue_t *q, const input_event_t *ev);

/* ---- Pure debouncer, src/lib/input_debounce.c ---- */

#define INPUT_DEBOUNCE_LINES 32

typedef struct {
    uint32_t active_low;    /* bit n set: line n is pressed when it reads 0 */
    uint32_t state;         /* debounced pressed state, bit per line */
    uint8_t count[INPUT_DEBOUNCE_LINES];
    uint8_t stable;         /* samples a new level must hold */
} input_debounce_t;

/* `stable_samples` 1 to 255. Every line starts released. */
void input_debounce_init(input_debounce_t *d, unsigned stable_samples, uint32_t active_low);

/*
 * Feeds one sample of every line, bit n for line n. Stores the lines that
 * became pressed and released at this sample. Returns the debounced state.
 */
uint32_t input_debounce_feed(input_debounce_t *d, uint32_t levels,
                             uint32_t *pressed, uint32_t *released);

/* Samples needed to cover `ms` at `hz` samples per second, rounded up, 1 to 255. */
unsigned input_debounce_samples(unsigned ms, unsigned hz);

/* ---- Pure quadrature decoder, src/lib/input_quad.c ---- */

typedef struct {
    uint8_t prev;       /* last A/B state, (A << 1) | B */
    int8_t acc;         /* transitions not yet turned into a detent */
    uint8_t per_detent; /* transitions per detent: 1, 2 or 4 */
    uint8_t reverse;
    uint32_t invalid;   /* rejected double transitions */
} input_quad_t;

/* `per_detent` 1, 2 or 4 (other values become 4). a, b: initial levels. */
void input_quad_init(input_quad_t *q, unsigned per_detent, int reverse, int a, int b);

/* Feeds one A/B sample. Returns the detent steps completed: -1, 0 or 1. */
int input_quad_feed(input_quad_t *q, int a, int b);

/* ---- Pure ANSI/VT input parser, src/lib/input_ansi.c ---- */

#define INPUT_ANSI_OUT_MAX  2   /* events one call can produce */

typedef struct {
    uint8_t state;
    uint8_t last_cr;
    uint8_t nparam;
    uint16_t param;
    uint32_t esc_ms;
    uint32_t timeout_ms;
} input_ansi_t;

/* `esc_timeout_ms`: how long a lone ESC waits before it counts as BACK. */
void input_ansi_init(input_ansi_t *p, uint32_t esc_timeout_ms);

/*
 * Feeds one byte received at `now_ms`. Writes up to INPUT_ANSI_OUT_MAX
 * events to out[] and returns their count. When the ESC timeout already
 * passed at `now_ms`, the pending ESC or sequence is resolved first, as
 * input_ansi_poll() does, and the byte starts afresh. Keys come out as INPUT_EV_KEY
 * with value INPUT_PRESS; the caller adds the release. source and t_ms
 * are left 0.
 */
int input_ansi_feed(input_ansi_t *p, uint8_t byte, uint32_t now_ms, input_event_t *out);

/*
 * Call it when no byte came. Resolves a lone ESC into BACK once the
 * timeout passed, and drops an unfinished sequence. Returns 0 or 1.
 */
int input_ansi_poll(input_ansi_t *p, uint32_t now_ms, input_event_t *out);

/* ---- Pure held-key state, src/lib/input_hold.c ---- */

#define INPUT_HOLD_SOURCES  4   /* sources 0 to 3 are tracked, others ignored */

/*
 * One bit per key code below 32 for each source. A key is down while any
 * source holds it. A press from a second source on a key already down is
 * still delivered as an event; the state is the OR of all sources.
 */
typedef struct {
    uint32_t held[INPUT_HOLD_SOURCES];
    uint16_t rep_code;  /* key to auto-repeat, 0 for none */
    uint8_t rep_source; /* source the repeat events carry */
} input_hold_t;

void input_hold_init(input_hold_t *h);

/*
 * Applies a KEY event; other events are ignored. A press makes its key the
 * repeat key and returns 1 so the caller restarts the repeat delay. A
 * release clears only that source's hold. If another source still holds
 * the repeat key, the repeat goes on from that source without a restart.
 * A release from a source that did not hold the key changes nothing.
 * Returns 0 except for a press.
 */
int input_hold_update(input_hold_t *h, const input_event_t *ev);

/* Non-zero while any source holds `code`. */
int input_hold_down(const input_hold_t *h, unsigned code);

/* ---- Pure input state, src/lib/input_core.c ---- */

/*
 * The queue, the held keys, the dial totals and the overflow policy that
 * src/drv/input.c keeps. The caller provides mutual exclusion.
 */
typedef struct {
    input_queue_t queue;
    input_hold_t hold;
    uint32_t dial_total[INPUT_DIAL_COUNT];  /* wraps; read through input_core_dial_total() */
    int policy;                             /* INPUT_DROP_* */
} input_core_t;

/* Empties everything and sets INPUT_DROP_NEWEST. */
void input_core_init(input_core_t *c);

/*
 * Updates the held keys and the dial totals, then queues the event under
 * the policy. Sets *pressed (if not 0) to input_hold_update()'s result.
 * Returns 0, INPUT_ERR_FULL (INPUT_DROP_NEWEST) or INPUT_DROPPED_OLDEST.
 */
int input_core_push(input_core_t *c, const input_event_t *ev, int *pressed);

/* Running sum of the steps of `dial`, wrapping at 32 bits. 0 for dial >= INPUT_DIAL_COUNT. */
int32_t input_core_dial_total(const input_core_t *c, unsigned dial);

/* ---- Queue and key state, src/drv/input.c ---- */

/*
 * Empties the queue and clears counters, key state, dial totals and
 * repeat. Sets the overflow policy back to INPUT_DROP_NEWEST. Main
 * context, IRQs masked or not.
 */
int input_init(void);

/*
 * Adds an event and stamps t_ms. Any context. For KEY events it also
 * updates the key state and the repeat timer, for DIAL events the dial
 * total, even when the queue drops an event. Returns 0, INPUT_ERR_FULL
 * (INPUT_DROP_NEWEST: this event was dropped) or INPUT_DROPPED_OLDEST
 * (INPUT_DROP_OLDEST: this event was queued, the oldest one dropped).
 */
int input_push(const input_event_t *ev);

/* Runs the main-loop backends, then takes the oldest event. Returns 1 or 0. Main context. */
int input_poll(input_event_t *ev);

#define INPUT_WAIT_FOREVER  0xffffffffu

/*
 * Like input_poll(), but waits up to `timeout_ms` for an event. Sleeps in
 * wfi between checks when CPU IRQs are on and the tick runs. Main context.
 *
 * The timeout needs a running system timer. When the timer does not
 * advance, a finite timeout never expires: the call returns only when an
 * event comes. Check the timer first, or use input_poll() in a loop with
 * your own bound.
 */
int input_wait(input_event_t *ev, uint32_t timeout_ms);

unsigned input_pending(void);
void input_flush(void);

/* Counters since input_init(). Either pointer may be 0. */
void input_stats(uint32_t *pushed, uint32_t *dropped);

/* Non-zero while any source holds key `code`. See input_hold_update(). */
int input_key_down(unsigned code);

/*
 * Signed detent steps of `dial` since input_init(), counting events the
 * queue dropped. Wraps as a 32-bit two's-complement counter: take
 * differences. 0 for dial >= INPUT_DIAL_COUNT. Any context.
 */
int32_t input_dial_total(unsigned dial);

/* INPUT_DROP_NEWEST (default) or INPUT_DROP_OLDEST. Other values are ignored. Any context. */
void input_set_overflow(int policy);

/* Events dropped since input_init(), under either policy. Any context. */
unsigned input_dropped(void);

/*
 * Auto-repeat for a held key: the first repeat after `delay_ms`, then one
 * every `rate_ms`. delay_ms 0 turns it off (the default). Generated from a
 * tick callback, so the tick must run. Returns 0, INPUT_ERR_ARG, or
 * INPUT_ERR_SPACE when no tick callback slot is free.
 */
int input_set_repeat(uint32_t delay_ms, uint32_t rate_ms);

/* For backends: a function input_poll() calls from main context. Max 4. */
int input_add_poller(void (*fn)(void));

/* Name of a key code ("up", "f1", ...), or 0 if unknown. */
const char *input_key_name(unsigned code);

/* ---- GPIO backend, src/drv/input_gpio.c ---- */

#define INPUT_GPIO_MAX_BUTTONS  16
#define INPUT_GPIO_MAX_DIALS    2
#define INPUT_GPIO_NO_PIN       (-1)
#define INPUT_GPIO_GUARD_MS     100

enum {
    INPUT_GPIO_OFF = 0,         /* input_gpio_start() not called or failed */
    INPUT_GPIO_CHECKING,        /* stuck-line guard still sampling */
    INPUT_GPIO_ACTIVE,
    INPUT_GPIO_DISABLED_STUCK,  /* every button read active for the whole guard */
};

/* `active_level` 0 (pressed reads low) or 1. `pull` a GPIO_PULL_* code. Before start. */
int input_gpio_add_button(unsigned pin, unsigned code, int active_level, unsigned pull);

/*
 * A quadrature dial on pin_a and pin_b with pull-ups, and an optional
 * active-low push button (INPUT_GPIO_NO_PIN for none). Swap the pins to
 * reverse the direction. Dial index is the order of the calls. Before start.
 */
int input_gpio_add_dial(unsigned pin_a, unsigned pin_b, int push_pin, unsigned per_detent);

/*
 * Configures the pins and starts sampling from a tick callback. `tick_hz`
 * must be the running tick rate: tick_init(tick_hz) is called and must
 * return 0. Returns 0, INPUT_ERR_NO_IRQ, INPUT_ERR_TICK, INPUT_ERR_SPACE
 * or INPUT_ERR_STATE. Main context.
 */
int input_gpio_start(uint32_t tick_hz, unsigned debounce_ms);

int input_gpio_status(void);
const char *input_gpio_status_name(int status);

/* ---- Serial backend on the PL011 console, src/drv/input_serial.c ---- */

#define INPUT_SERIAL_MAIN   0   /* read from input_poll() */
#define INPUT_SERIAL_TICK   1   /* read from a tick callback */
#define INPUT_SERIAL_ESC_MS 100

/* Starts reading the console UART through the ANSI parser. Call uart_init() first. */
int input_serial_start(int mode);

/* ---- Script backend on the mini UART, src/drv/input_script.c ---- */

#define INPUT_SCRIPT_LINE_MAX 128

/*
 * Starts reading script lines from the mini UART. Call uart_mini_init()
 * first: before it, returns INPUT_ERR_NO_UART.
 */
int input_script_start(void);

/* Words a project command line may hold, its name included. */
#define INPUT_SCRIPT_ARGS_MAX 8

/*
 * What a project handler gets to answer one line.
 *   write   Writes text to the script UART. Each line the handler writes
 *           reaches the host with "| " in front, so it never looks like
 *           a status line. A line ends at '\n'.
 *   reason  Set it before returning a negative value: the backend
 *           answers "err <reason>". NULL gives "err failed".
 */
typedef struct input_script_reply {
    void (*write)(const char *text);
    const char *reason;
} input_script_reply_t;

/*
 * A project command. argv[0] is the command name, argc counts at most
 * INPUT_SCRIPT_ARGS_MAX words. The strings live until the handler returns.
 * Return 0 for "ok", a negative value for "err <reason>".
 */
typedef int (*input_script_handler_t)(int argc, char *argv[], input_script_reply_t *reply,
                                      void *ctx);

/*
 * Passes each line whose first word is not a built-in command to fn, with
 * ctx. NULL removes the handler: such lines then get "err unknown command".
 * The handler runs in the context that polls the backend, input_poll() or
 * input_wait() in the main loop, never in an interrupt. While it runs no
 * other script line is read. It may call the input API, but not
 * input_poll() or input_wait().
 */
void input_script_set_handler(input_script_handler_t fn, void *ctx);


/* ---- Pure script line reader, src/lib/input_line.c ---- */

/* Blanks are spaces and tabs. */
typedef struct input_line {
    char buf[INPUT_SCRIPT_LINE_MAX];
    unsigned len;
    int overflow;
    char *line;     /* the line after INPUT_LINE_READY, inside buf */
} input_line_t;

#define INPUT_LINE_NONE     0   /* no line yet, or a blank line */
#define INPUT_LINE_READY    1   /* l->line holds the trimmed line, not empty */
#define INPUT_LINE_LONG     2   /* the line was over INPUT_SCRIPT_LINE_MAX - 1 bytes */

void input_line_init(input_line_t *l);

/*
 * Feeds one byte. CR or LF ends a line. A blank line gives
 * INPUT_LINE_NONE. A long line gives INPUT_LINE_LONG once, at its end,
 * and the next byte starts a new line. l->line stays valid until the
 * next call.
 */
int input_line_feed(input_line_t *l, char c);

/* Cuts leading blanks, trailing blanks and CR. Returns the new start. */
char *input_line_trim(char *s);

/* Ends the first word of s in place. Returns what follows its blanks. */
char *input_line_word(char *s);

/*
 * Splits s in place at runs of blanks into argv[0..argc-1] and sets
 * argv[argc] to NULL: argv holds max + 1 entries. Returns argc, 0 for
 * a blank s, or -1 for more than max words.
 */
int input_line_split(char *s, char *argv[], int max);

#endif
