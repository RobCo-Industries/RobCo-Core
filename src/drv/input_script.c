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
 */

#define DIAL_MAX    1000000

static char line[INPUT_SCRIPT_LINE_MAX];
static unsigned len;
static int overflow;
static int waiting;
static uint32_t wait_until;
static int started;
static int full;    /* a push of the current line was dropped */

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

/* Splits "word rest" in place. Returns the rest, after one space, or "". */
static char *split(char *s)
{
    while (*s && *s != ' ') {
        s++;
    }
    if (*s == ' ') {
        *s++ = 0;
    }
    return s;
}

/* Returns the reply, or 0 when the reply comes later (wait). */
static const char *run(char *cmd)
{
    static char buf[48];
    char *arg = split(cmd);
    int32_t n;

    if (k_strcmp(cmd, "key") == 0) {
        char *val = split(arg);
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
    } else {
        return "err unknown command";
    }
    return full ? "err full" : "ok";
}

static void end_line(void)
{
    const char *r;

    if (overflow) {
        r = "err line too long";
    } else {
        line[len] = 0;
        full = 0;
        r = run(line);
    }
    if (r) {
        reply(r);
    }
    len = 0;
    overflow = 0;
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
        if (c == '\r' || c == '\n') {
            if (len || overflow) {
                end_line();
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            overflow = 1;
        }
    }
}

int input_script_start(void)
{
    int ret;

    if (started) {
        return INPUT_ERR_STATE;
    }
    len = 0;
    overflow = 0;
    waiting = 0;
    ret = input_add_poller(script_service);
    if (ret == 0) {
        started = 1;
    }
    return ret;
}
