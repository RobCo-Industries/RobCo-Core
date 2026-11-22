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

void robco_main(void)
{
    const robco_board_t *b = board_get();
    input_event_t ev;
    uint32_t pushed, dropped;
    unsigned i;
    int mini, gpio, rep = 0;

    uart_init(115200);
    k_printf("\n@RC stage=input ev=boot soc=%s base=0x%08x\n", b->soc_name,
             (unsigned)b->periph_base);

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
#if defined(INPUT_DEMO_REPEAT) && INPUT_DEMO_REPEAT
    rep = input_set_repeat(REPEAT_DELAY_MS, REPEAT_RATE_MS);
#endif
    input_serial_start(INPUT_SERIAL_MAIN);
    input_script_start();
    k_printf("@RC stage=input ev=init mini=%d gpio=%d repeat=%d\n", mini, gpio, rep);

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
