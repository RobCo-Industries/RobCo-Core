#include <stdint.h>

#include <robco/input.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

/*
 * Reads the PL011 console through the ANSI parser. Nothing reads the UART
 * until input_serial_start(): code that calls uart_poll() itself keeps
 * every byte until then.
 */

#define BYTES_PER_CALL  64  /* bounds the time spent in one call */

static input_ansi_t parser;
static int started;

static void push_out(const input_event_t *out, int n)
{
    input_event_t e;
    int i;

    for (i = 0; i < n; i++) {
        e = out[i];
        e.source = INPUT_SRC_SERIAL;
        input_push(&e);
        if (e.type == INPUT_EV_KEY) {
            /* A terminal sends a key once: report a press and a release. */
            e.value = INPUT_RELEASE;
            input_push(&e);
        }
    }
}

static void serial_service(void)
{
    input_event_t out[INPUT_ANSI_OUT_MAX];
    uint32_t now = timer_now_ms();
    unsigned i;
    int c;

    for (i = 0; i < BYTES_PER_CALL; i++) {
        c = uart_poll();
        if (c < 0) {
            break;
        }
        push_out(out, input_ansi_feed(&parser, (uint8_t)c, now, out));
    }
    push_out(out, input_ansi_poll(&parser, now, out));
}

int input_serial_start(int mode)
{
    uint32_t clock, baud;
    int ret;

    if (started) {
        return INPUT_ERR_STATE;
    }
    if (uart_get_config(&clock, &baud) != 0) {
        return INPUT_ERR_STATE;
    }
    input_ansi_init(&parser, INPUT_SERIAL_ESC_MS);
    if (mode == INPUT_SERIAL_MAIN) {
        ret = input_add_poller(serial_service);
    } else if (mode == INPUT_SERIAL_TICK) {
        ret = tick_register(serial_service) ? INPUT_ERR_SPACE : 0;
    } else {
        return INPUT_ERR_ARG;
    }
    if (ret == 0) {
        started = 1;
    }
    return ret;
}
