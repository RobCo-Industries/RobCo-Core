#include <stdint.h>

#include <robco/gpio.h>
#include <robco/input.h>
#include <robco/irq.h>
#include <robco/tick.h>

/*
 * Buttons and dials sampled from a tick callback. Buttons go through the
 * debouncer, dials through the quadrature decoder. The first 100 ms are a
 * guard: if every button reads active for all of it, the lines are taken
 * as not wired and the backend stops.
 */

typedef struct {
    uint8_t pin;
    uint8_t code;
} button_t;

typedef struct {
    uint8_t pin_a;
    uint8_t pin_b;
    uint8_t per_detent;
    input_quad_t quad;
} dial_t;

static button_t buttons[INPUT_GPIO_MAX_BUTTONS];
static unsigned nbuttons;
static uint8_t pulls[INPUT_GPIO_MAX_BUTTONS];
static uint32_t active_low;

static dial_t dials[INPUT_GPIO_MAX_DIALS];
static unsigned ndials;

static input_debounce_t deb;
static volatile int status = INPUT_GPIO_OFF;
static int started;
static uint32_t guard_left;
static int guard_all_active;

static int push(uint8_t type, uint16_t code, int32_t value)
{
    input_event_t e;

    e.type = type;
    e.source = INPUT_SRC_GPIO;
    e.code = code;
    e.value = value;
    e.t_ms = 0;
    return input_push(&e);
}

static int add_button(unsigned pin, unsigned code, int active_level, unsigned pull)
{
    unsigned i = nbuttons;

    buttons[i].pin = (uint8_t)pin;
    buttons[i].code = (uint8_t)code;
    pulls[i] = (uint8_t)pull;
    if (active_level == 0) {
        active_low |= (uint32_t)1 << i;
    }
    nbuttons++;
    return 0;
}

int input_gpio_add_button(unsigned pin, unsigned code, int active_level, unsigned pull)
{
    if (started) {
        return INPUT_ERR_STATE;
    }
    if (pin >= GPIO_PIN_COUNT || code == INPUT_KEY_NONE || code >= INPUT_KEY_COUNT ||
        (active_level != 0 && active_level != 1) || pull > GPIO_PULL_UP) {
        return INPUT_ERR_ARG;
    }
    if (nbuttons >= INPUT_GPIO_MAX_BUTTONS) {
        return INPUT_ERR_SPACE;
    }
    return add_button(pin, code, active_level, pull);
}

int input_gpio_add_dial(unsigned pin_a, unsigned pin_b, int push_pin, unsigned per_detent)
{
    dial_t *d;

    if (started) {
        return INPUT_ERR_STATE;
    }
    if (pin_a >= GPIO_PIN_COUNT || pin_b >= GPIO_PIN_COUNT || pin_a == pin_b ||
        (push_pin != INPUT_GPIO_NO_PIN && (push_pin < 0 || push_pin >= GPIO_PIN_COUNT)) ||
        (per_detent != 1 && per_detent != 2 && per_detent != 4)) {
        return INPUT_ERR_ARG;
    }
    if (ndials >= INPUT_GPIO_MAX_DIALS ||
        (push_pin != INPUT_GPIO_NO_PIN && nbuttons >= INPUT_GPIO_MAX_BUTTONS)) {
        return INPUT_ERR_SPACE;
    }
    d = &dials[ndials++];
    d->pin_a = (uint8_t)pin_a;
    d->pin_b = (uint8_t)pin_b;
    d->per_detent = (uint8_t)per_detent;
    if (push_pin != INPUT_GPIO_NO_PIN) {
        add_button((unsigned)push_pin, INPUT_KEY_DIAL_PUSH, 0, GPIO_PULL_UP);
    }
    return 0;
}

static uint32_t read_buttons(void)
{
    uint32_t levels = 0;
    unsigned i;

    for (i = 0; i < nbuttons; i++) {
        if (gpio_read(buttons[i].pin) > 0) {
            levels |= (uint32_t)1 << i;
        }
    }
    return levels;
}

static void push_edges(uint32_t mask, int32_t value)
{
    unsigned i;

    for (i = 0; i < nbuttons; i++) {
        if (mask & ((uint32_t)1 << i)) {
            push(INPUT_EV_KEY, buttons[i].code, value);
        }
    }
}

/* Tick callback, IRQ context. */
static void gpio_sample(void)
{
    uint32_t all = nbuttons ? (((uint32_t)1 << nbuttons) - 1) : 0;
    uint32_t levels, state, pressed, released;
    unsigned i;
    int st = status;

    if (st != INPUT_GPIO_CHECKING && st != INPUT_GPIO_ACTIVE) {
        return;
    }
    levels = read_buttons();
    state = input_debounce_feed(&deb, levels, &pressed, &released);

    for (i = 0; i < ndials; i++) {
        int step = input_quad_feed(&dials[i].quad, gpio_read(dials[i].pin_a) > 0,
                                   gpio_read(dials[i].pin_b) > 0);

        if (step && st == INPUT_GPIO_ACTIVE) {
            push(INPUT_EV_DIAL, (uint16_t)i, step);
        }
    }

    if (st == INPUT_GPIO_ACTIVE) {
        push_edges(pressed, INPUT_PRESS);
        push_edges(released, INPUT_RELEASE);
        return;
    }

    if (((levels ^ active_low) & all) != all || nbuttons == 0) {
        guard_all_active = 0;
    }
    if (--guard_left != 0) {
        return;
    }
    if (guard_all_active) {
        status = INPUT_GPIO_DISABLED_STUCK;
        return;
    }
    status = INPUT_GPIO_ACTIVE;
    /* Buttons already held when the guard ends are reported now. */
    push_edges(state, INPUT_PRESS);
}

int input_gpio_start(uint32_t tick_hz, unsigned debounce_ms)
{
    uint32_t guard;
    unsigned i;

    if (started) {
        return INPUT_ERR_STATE;
    }
    if (!irq_ready()) {
        return INPUT_ERR_NO_IRQ;
    }
    if (tick_init(tick_hz) != 0) {
        return INPUT_ERR_TICK;
    }
    for (i = 0; i < nbuttons; i++) {
        gpio_set_function(buttons[i].pin, GPIO_FN_INPUT);
        gpio_set_pull(buttons[i].pin, pulls[i]);    /* fails on BCM2711: pulls stay as they are */
    }
    for (i = 0; i < ndials; i++) {
        gpio_set_function(dials[i].pin_a, GPIO_FN_INPUT);
        gpio_set_function(dials[i].pin_b, GPIO_FN_INPUT);
        gpio_set_pull(dials[i].pin_a, GPIO_PULL_UP);
        gpio_set_pull(dials[i].pin_b, GPIO_PULL_UP);
        input_quad_init(&dials[i].quad, dials[i].per_detent, 0,
                        gpio_read(dials[i].pin_a) > 0, gpio_read(dials[i].pin_b) > 0);
    }
    input_debounce_init(&deb, input_debounce_samples(debounce_ms, tick_hz), active_low);

    guard = ((uint64_t)INPUT_GPIO_GUARD_MS * tick_hz + 999) / 1000;
    guard_left = guard ? guard : 1;
    guard_all_active = 1;
    status = INPUT_GPIO_CHECKING;
    if (tick_register(gpio_sample) != 0) {
        status = INPUT_GPIO_OFF;
        return INPUT_ERR_SPACE;
    }
    started = 1;
    return 0;
}

int input_gpio_status(void)
{
    return status;
}

const char *input_gpio_status_name(int st)
{
    switch (st) {
    case INPUT_GPIO_OFF:            return "off";
    case INPUT_GPIO_CHECKING:       return "checking";
    case INPUT_GPIO_ACTIVE:         return "active";
    case INPUT_GPIO_DISABLED_STUCK: return "disabled_stuck";
    default:                        return "unknown";
    }
}
