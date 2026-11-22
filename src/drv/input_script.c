#include <stdint.h>

#include <robco/input.h>
#include <robco/klibc.h>
#include <robco/timer.h>
#include <robco/uart_mini.h>

/*
 * Line commands on the mini UART, read from input_poll(). Each line gets
 * one reply, "ok" or "err <reason>":
 *
 *   key <name> [1|0]   press and release, or press only, or release only
 *   dial <+n|-n>       one DIAL event on dial 0 with value n
 *   char <c>           one CHAR event
 *   text <string>      one CHAR event per character
 *   wait <ms>          holds the next lines for at least ms milliseconds
 *   stats              "ok pushed=<n> dropped=<n>"
 *
 * Any other first word goes to the handler of input_script_set_handler(),
 * if one is set. Its output lines come first, each with "| " in front,
 * then the status line.
 *
 * src/lib/input_line.c reads the lines. Each line loses its leading and
 * trailing blanks first, so built-in and project commands see the same
 * words. A blank line gets no reply. A line too long for the buffer gets
 * one "err line too long" when it ends, and the next line reads normally.
 */

#define DIAL_MAX    1000000

static input_line_t rd;
static int waiting;
static uint32_t wait_until;
static int started;
static int full;    /* a push of the current line was dropped */
static input_script_handler_t handler;
static void *handler_ctx;
static int out_bol = 1; /* the next handler byte starts a line */

static void reply(const char *s)
{
    uart_mini_puts(s);
    uart_mini_putc('\n');
}

static void push(uint8_t type, uint16_t code, int32_t value)
{
    input_event_t e;

    e.type = type;
    e.source = INPUT_SRC_SCRIPT;
    e.code = code;
    e.value = value;
    e.t_ms = 0;
    if (input_push(&e) != 0) {
        full = 1;
    }
}

/* Parses an optionally signed decimal number that fills all of s. */
static int parse_int(const char *s, int32_t *out)
{
    int neg = 0;
    int32_t v = 0;

    if (*s == '+' || *s == '-') {
        neg = *s == '-';
        s++;
    }
    if (!*s) {
        return -1;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000) {
            return -1;
        }
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

static unsigned key_code(const char *name)
{
    unsigned c;

    for (c = 1; c < INPUT_KEY_COUNT; c++) {
        if (k_strcmp(input_key_name(c), name) == 0) {
            return c;
        }
    }
    return INPUT_KEY_NONE;
}

/* reply->write for handlers: prefixes every line with "| ". */
static void handler_write(const char *text)
{
    for (; text && *text; text++) {
        if (out_bol) {
            uart_mini_puts("| ");
            out_bol = 0;
        }
        uart_mini_putc(*text);
        if (*text == '\n') {
            out_bol = 1;
        }
    }
}

/* Runs the project handler on the whole line, split into words. */
static const char *run_handler(char *cmd)
{
    static char buf[INPUT_SCRIPT_LINE_MAX + 8];
    char *argv[INPUT_SCRIPT_ARGS_MAX + 1];
    input_script_reply_t r;
    int argc = input_line_split(cmd, argv, INPUT_SCRIPT_ARGS_MAX);
    int ret;

    if (argc < 0) {
        return "err too many words";
    }
    r.write = handler_write;
    r.reason = 0;
    out_bol = 1;
    ret = handler(argc, argv, &r, handler_ctx);
    if (!out_bol) {
        uart_mini_putc('\n');
        out_bol = 1;
    }
    if (ret >= 0) {
        return "ok";
    }
    k_snprintf(buf, sizeof(buf), "err %s", r.reason && *r.reason ? r.reason : "failed");
    return buf;
}

void input_script_set_handler(input_script_handler_t fn, void *ctx)
{
    handler = 0;
    handler_ctx = ctx;
    handler = fn;
}

/* Runs a trimmed, non-empty line. Returns the reply, or 0 when the reply
 * comes later (wait). */
static const char *run(char *cmd)
{
    static char buf[48];
    static char copy[INPUT_SCRIPT_LINE_MAX];
    char *arg;
    int32_t n;

    k_strncpy(copy, cmd, sizeof(copy));
    arg = input_line_word(cmd);

    if (k_strcmp(cmd, "key") == 0) {
        char *val = input_line_word(arg);
        unsigned code = key_code(arg);

        if (code == INPUT_KEY_NONE) {
            return "err unknown key";
        }
        if (*val == 0) {
            push(INPUT_EV_KEY, (uint16_t)code, INPUT_PRESS);
            push(INPUT_EV_KEY, (uint16_t)code, INPUT_RELEASE);
        } else if (k_strcmp(val, "1") == 0 || k_strcmp(val, "0") == 0) {
            push(INPUT_EV_KEY, (uint16_t)code, val[0] == '1' ? INPUT_PRESS : INPUT_RELEASE);
        } else {
            return "err bad value";
        }
    } else if (k_strcmp(cmd, "dial") == 0) {
        if (parse_int(arg, &n) != 0 || n == 0 || n > DIAL_MAX || n < -DIAL_MAX) {
            return "err bad steps";
        }
        push(INPUT_EV_DIAL, 0, n);
    } else if (k_strcmp(cmd, "char") == 0) {
        if (arg[0] == 0 || arg[1] != 0) {
            return "err one character";
        }
        push(INPUT_EV_CHAR, (uint8_t)arg[0], 0);
    } else if (k_strcmp(cmd, "text") == 0) {
        for (; *arg; arg++) {
            push(INPUT_EV_CHAR, (uint8_t)*arg, 0);
        }
    } else if (k_strcmp(cmd, "wait") == 0) {
        if (parse_int(arg, &n) != 0 || n < 0 || arg[0] == '+' || arg[0] == '-') {
            return "err bad time";
        }
        waiting = 1;
        wait_until = timer_now_ms() + (uint32_t)n;
        return 0;
    } else if (k_strcmp(cmd, "stats") == 0) {
        uint32_t pushed, dropped;

        input_stats(&pushed, &dropped);
        k_snprintf(buf, sizeof(buf), "ok pushed=%u dropped=%u",
                   (unsigned)pushed, (unsigned)dropped);
        return buf;
    } else if (handler != 0) {
        return run_handler(copy);
    } else {
        return "err unknown command";
    }
    return full ? "err full" : "ok";
}

/* Main context, from input_poll(). */
static void script_service(void)
{
    int c;

    if (waiting) {
        if ((int32_t)(timer_now_ms() - wait_until) < 0) {
            return;
        }
        waiting = 0;
        reply("ok");
    }
    while (!waiting && (c = uart_mini_poll()) >= 0) {
        const char *r = 0;

        switch (input_line_feed(&rd, (char)c)) {
        case INPUT_LINE_LONG:
            r = "err line too long";
            break;
        case INPUT_LINE_READY:
            full = 0;
            r = run(rd.line);
            break;
        default:
            continue;
        }
        if (r) {
            reply(r);
        }
    }
}

int input_script_start(void)
{
    int ret;

    if (started) {
        return INPUT_ERR_STATE;
    }
    if (!uart_mini_ready()) {
        return INPUT_ERR_NO_UART;
    }
    input_line_init(&rd);
    waiting = 0;
    ret = input_add_poller(script_service);
    if (ret == 0) {
        started = 1;
    }
    return ret;
}
