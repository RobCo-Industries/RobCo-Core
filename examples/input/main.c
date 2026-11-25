#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/input.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>
#include <robco/uart_mini.h>

#define TICK_HZ         100
#define DEBOUNCE_MS     20
#define REPEAT_DELAY_MS 300
#define REPEAT_RATE_MS  100

static const struct {
    unsigned pin;
    unsigned code;
} buttons[] = {
    { INPUT_PIN_UP, INPUT_KEY_UP },
    { INPUT_PIN_DOWN, INPUT_KEY_DOWN },
    { INPUT_PIN_LEFT, INPUT_KEY_LEFT },
    { INPUT_PIN_RIGHT, INPUT_KEY_RIGHT },
    { INPUT_PIN_SELECT, INPUT_KEY_SELECT },
    { INPUT_PIN_BACK, INPUT_KEY_BACK },
};

static const char *type_name(unsigned type)
{
    switch (type) {
    case INPUT_EV_KEY:  return "key";
    case INPUT_EV_DIAL: return "dial";
    case INPUT_EV_CHAR: return "char";
    default:            return "unknown";
    }
}

static const char *source_name(unsigned src)
{
    switch (src) {
    case INPUT_SRC_GPIO:   return "gpio";
    case INPUT_SRC_SERIAL: return "serial";
    case INPUT_SRC_SCRIPT: return "script";
    default:               return "unknown";
    }
}

static void print_event(const input_event_t *ev)
{
    const char *name = ev->type == INPUT_EV_KEY ? input_key_name(ev->code) : 0;

    k_printf("@RC stage=input ev=event type=%s src=%s code=",
             type_name(ev->type), source_name(ev->source));
    if (name) {
        k_printf("%s", name);
    } else {
        k_printf("%u", (unsigned)ev->code);
    }
    k_printf(" value=%d t_ms=%u", (int)ev->value, (unsigned)ev->t_ms);
    if (ev->type == INPUT_EV_KEY) {
        k_printf(" down=%d", input_key_down(ev->code) ? 1 : 0);
    }
    k_printf("\n");
}

/*
 * "burst <n>": pushes n DIAL events in one go, before the main loop can
 * take any. Event i (1 to n) is on dial i % 2 with value i, negated when
 * i is a multiple of 3. Answers "pushed=<n> full=<n> oldest=<n>": the
 * input_push() results.
 */
static int burst(int argc, char *argv[], input_script_reply_t *reply)
{
    input_event_t e;
    char line[64];
    unsigned n = 0, i, full = 0, oldest = 0;
    const char *p;
    int r;

    if (argc != 2 || argv[1][0] == 0) {
        reply->reason = "burst takes a count";
        return -1;
    }
    for (p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9' || n > 1000) {
            reply->reason = "bad count";
            return -1;
        }
        n = n * 10 + (unsigned)(*p - '0');
    }
    for (i = 1; i <= n; i++) {
        e.type = INPUT_EV_DIAL;
        e.source = INPUT_SRC_SCRIPT;
        e.code = (uint16_t)(i % 2);
        e.value = i % 3 ? (int32_t)i : -(int32_t)i;
        e.t_ms = 0;
        r = input_push(&e);
        full += r == INPUT_ERR_FULL;
        oldest += r == INPUT_DROPPED_OLDEST;
    }
    k_snprintf(line, sizeof(line), "pushed=%u full=%u oldest=%u", n, full, oldest);
    reply->write(line);
    return 0;
}

/*
 * Tick timing since the last "ticks": the longest gap between two tick
 * callbacks, and the spread of their lateness against the tick's grid,
 * both in us. The guest samples the GPIO lines on these callbacks.
 */
static volatile uint32_t tick_last_lo, tick_gap_max, tick_ref;
static volatile int32_t tick_late_min, tick_late_max;
static volatile int tick_fresh = 1;

static void tick_timing(void)
{
    uint32_t now = timer_now_lo();
    /* A constant plus this callback's lateness: count * period is the grid. */
    uint32_t off = now - tick_count() * (1000000u / TICK_HZ);
    int32_t late;

    if (tick_fresh) {
        tick_ref = off;
        tick_late_min = 0;
        tick_late_max = 0;
        tick_gap_max = 0;
        tick_fresh = 0;
    } else if (now - tick_last_lo > tick_gap_max) {
        tick_gap_max = now - tick_last_lo;
    }
    tick_last_lo = now;
    late = (int32_t)(off - tick_ref);
    if (late < tick_late_min) {
        tick_late_min = late;
    }
    if (late > tick_late_max) {
        tick_late_max = late;
    }
}

/*
 * Project commands on the script channel. "board" answers two lines,
 * then ok. "totals" answers "dial0=<n> dial1=<n> dropped=<n>". "ticks"
 * answers "count=<n> missed=<n> maxgap=<us> jitter=<us>": tick_count(),
 * tick_missed() and the tick timing above since the last "ticks". "burst"
 * is above. Other arguments and unknown commands answer err.
 */
static int script_command(int argc, char *argv[], input_script_reply_t *reply, void *ctx)
{
    const robco_board_t *b = ctx;
    char line[96];

    if (k_strcmp(argv[0], "burst") == 0) {
        return burst(argc, argv, reply);
    }
    if (k_strcmp(argv[0], "totals") == 0) {
        k_snprintf(line, sizeof(line), "dial0=%d dial1=%d dropped=%u",
                   (int)input_dial_total(0), (int)input_dial_total(1), input_dropped());
        reply->write(line);
        return 0;
    }
    if (k_strcmp(argv[0], "ticks") == 0) {
        k_snprintf(line, sizeof(line), "count=%u missed=%u maxgap=%u jitter=%u",
                   (unsigned)tick_count(), (unsigned)tick_missed(), (unsigned)tick_gap_max,
                   (unsigned)(tick_late_max - tick_late_min));
        reply->write(line);
        tick_fresh = 1;
        return 0;
    }
    if (k_strcmp(argv[0], "board") != 0) {
        reply->reason = "unknown command";
        return -1;
    }
    if (argc > 1) {
        reply->reason = "board takes no arguments";
        return -1;
    }
    k_snprintf(line, sizeof(line), "soc=%s\n", b->soc_name);
    reply->write(line);
    k_snprintf(line, sizeof(line), "base=0x%08x", (unsigned)b->periph_base);
    reply->write(line);     /* no '\n': the backend ends the line */
    return 0;
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    input_event_t ev;
    uint32_t pushed, dropped;
    unsigned i;
    int mini, gpio, rep = 0;
    int script, early_script, early_getc;

    uart_init(115200);
    k_printf("\n@RC stage=input ev=boot soc=%s base=0x%08x\n", b->soc_name,
             (unsigned)b->periph_base);

    /* Before uart_mini_init(): both must fail at once instead of hanging. */
    early_script = input_script_start();
    early_getc = uart_mini_getc();
    mini = uart_mini_init(115200);
    irq_init();
    tick_init(TICK_HZ);
    tick_register(tick_timing);
    irq_cpu_enable();

    input_init();
#if defined(INPUT_DEMO_DROP_OLDEST) && INPUT_DEMO_DROP_OLDEST
    input_set_overflow(INPUT_DROP_OLDEST);
#endif
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        input_gpio_add_button(buttons[i].pin, buttons[i].code, 0, GPIO_PULL_UP);
    }
    input_gpio_add_dial(INPUT_PIN_DIAL_A, INPUT_PIN_DIAL_B, INPUT_PIN_DIAL_PUSH, 4);
    gpio = input_gpio_start(TICK_HZ, DEBOUNCE_MS);
#if defined(INPUT_DEMO_REPEAT) && INPUT_DEMO_REPEAT
    rep = input_set_repeat(REPEAT_DELAY_MS, REPEAT_RATE_MS);
#endif
    input_serial_start(INPUT_SERIAL_MAIN);
    script = input_script_start();
    input_script_set_handler(script_command, (void *)b);
    k_printf("@RC stage=input ev=init mini=%d gpio=%d repeat=%d script=%d early_script=%d "
             "early_getc=%d\n", mini, gpio, rep, script, early_script, early_getc);

    /* The guard decides within about 100 ms of tick time. */
    while (input_gpio_status() == INPUT_GPIO_CHECKING) {
        timer_delay_ms(1);
    }
    k_printf("@RC stage=input ev=gpio status=%s\n",
             input_gpio_status_name(input_gpio_status()));
    k_printf("@RC stage=input ev=ready\n");
    uart_mini_puts("ready\n");

    for (;;) {
        if (!input_wait(&ev, 1000)) {
            continue;
        }
        print_event(&ev);
        if (ev.type == INPUT_EV_CHAR && ev.code == 'q') {
            input_stats(&pushed, &dropped);
            k_printf("@RC stage=input ev=stats pushed=%u dropped=%u pending=%u\n",
                     (unsigned)pushed, (unsigned)dropped, input_pending());
        }
    }
}
