#include <stdint.h>

#include <robco/input.h>
#include <robco/irq.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

/*
 * Reads the PL011 console through the ANSI parser. Nothing reads the UART
 * until input_serial_start(): code that calls uart_poll() itself keeps
 * every byte until then.
 *
 * The UART keeps no receive times, so every byte read in one call gets
 * the time of that call. An ESC counts from the call that read it. Bytes
 * read in the same call as the ESC still form a sequence with it. When a
 * later call starts after the timeout, the parser turns the ESC into BACK
 * before it takes the waiting bytes, even if they came in early.
 *
 * Under INPUT_DROP_NEWEST a byte stays in the UART until the queue has
 * room for every event it can make, INPUT_SERIAL_BYTE_EVENTS. The UART
 * receive FIFO then holds the rest: 16 bytes on the PL011. A sender with
 * no flow control overruns it once it is full. A call that stopped for
 * room restarts the ESC timeout when it reads again, so a sequence split
 * by the wait still parses.
 */

#define BYTES_PER_CALL  64  /* bounds the time spent in one call */

static input_ansi_t parser;
static int started;
static int stalled;     /* the last call left bytes for lack of room */

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
    uint32_t s;
    unsigned i;
    int c;

    if (stalled) {
        input_ansi_resume(&parser, now);
        stalled = 0;
    }
    for (i = 0; i < BYTES_PER_CALL; i++) {
        /* IRQs masked: no other producer takes the room between check and push. */
        s = irq_save();
        if (!input_room(INPUT_SERIAL_BYTE_EVENTS)) {
            irq_restore(s);
            stalled = 1;
            return;
        }
        c = uart_poll();
        if (c >= 0) {
            push_out(out, input_ansi_feed(&parser, (uint8_t)c, now, out));
        }
        irq_restore(s);
        if (c < 0) {
            break;
        }
    }
    s = irq_save();
    if (input_room(INPUT_SERIAL_BYTE_EVENTS)) {
        push_out(out, input_ansi_poll(&parser, now, out));
    }
    irq_restore(s);
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
