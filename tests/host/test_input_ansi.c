#include <stdint.h>

#include <robco/input.h>

#include "rc_test.h"

#define MAXEV 64

typedef struct {
    input_event_t ev[MAXEV];
    int n;
} evlog_t;

static void feed(input_ansi_t *p, evlog_t *log, const char *bytes, uint32_t now_ms)
{
    int i;

    for (i = 0; bytes[i]; i++) {
        input_event_t out[INPUT_ANSI_OUT_MAX];
        int k = input_ansi_feed(p, (uint8_t)bytes[i], now_ms, out);
        int j;

        CHECK(k >= 0 && k <= INPUT_ANSI_OUT_MAX);
        for (j = 0; j < k && log->n < MAXEV; j++) {
            log->ev[log->n++] = out[j];
        }
    }
}

static void poll(input_ansi_t *p, evlog_t *log, uint32_t now_ms)
{
    input_event_t out[INPUT_ANSI_OUT_MAX];
    int k = input_ansi_poll(p, now_ms, out);

    CHECK(k == 0 || k == 1);
    if (k == 1 && log->n < MAXEV) {
        log->ev[log->n++] = out[0];
    }
}

/* Checks that event i is a key press of `code`. */
#define CHECK_KEY(log, i, c)                                \
    do {                                                    \
        CHECK((log).n > (i));                               \
        CHECK_EQ((log).ev[i].type, INPUT_EV_KEY);           \
        CHECK_EQ((log).ev[i].code, (c));                    \
        CHECK_EQ((log).ev[i].value, INPUT_PRESS);           \
    } while (0)

#define CHECK_CHAR(log, i, c)                               \
    do {                                                    \
        CHECK((log).n > (i));                               \
        CHECK_EQ((log).ev[i].type, INPUT_EV_CHAR);          \
        CHECK_EQ((log).ev[i].code, (c));                    \
    } while (0)

TEST(ansi_arrows_csi_and_ss3)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1b[A\x1b[B\x1b[C\x1b[D\x1bOA\x1bOB\x1bOC\x1bOD", 0);
    CHECK_EQ(log.n, 8);
    CHECK_KEY(log, 0, INPUT_KEY_UP);
    CHECK_KEY(log, 1, INPUT_KEY_DOWN);
    CHECK_KEY(log, 2, INPUT_KEY_RIGHT);
    CHECK_KEY(log, 3, INPUT_KEY_LEFT);
    CHECK_KEY(log, 4, INPUT_KEY_UP);
    CHECK_KEY(log, 5, INPUT_KEY_DOWN);
    CHECK_KEY(log, 6, INPUT_KEY_RIGHT);
    CHECK_KEY(log, 7, INPUT_KEY_LEFT);
}

TEST(ansi_arrow_with_modifier)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1b[1;5A", 0);
    CHECK_EQ(log.n, 1);
    CHECK_KEY(log, 0, INPUT_KEY_UP);
}

TEST(ansi_enter_crlf_collapsed)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\r\n\r\n\n\r\r", 0);
    /* CR LF, CR LF, LF, CR, CR: five Enters. */
    CHECK_EQ(log.n, 5);
    CHECK_KEY(log, 0, INPUT_KEY_SELECT);
    CHECK_KEY(log, 4, INPUT_KEY_SELECT);
}

TEST(ansi_keypad_enter)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1bOM", 0);
    CHECK_EQ(log.n, 1);
    CHECK_KEY(log, 0, INPUT_KEY_SELECT);
}

TEST(ansi_backspace_and_del)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x7f\x08", 0);
    CHECK_EQ(log.n, 2);
    CHECK_KEY(log, 0, INPUT_KEY_BACK);
    CHECK_KEY(log, 1, INPUT_KEY_BACK);
}

TEST(ansi_function_keys)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1bOP\x1bOQ\x1bOR\x1bOS\x1b[11~\x1b[12~\x1b[13~\x1b[14~", 0);
    CHECK_EQ(log.n, 8);
    CHECK_KEY(log, 0, INPUT_KEY_F1);
    CHECK_KEY(log, 1, INPUT_KEY_F2);
    CHECK_KEY(log, 2, INPUT_KEY_F3);
    CHECK_KEY(log, 3, INPUT_KEY_F4);
    CHECK_KEY(log, 4, INPUT_KEY_F1);
    CHECK_KEY(log, 5, INPUT_KEY_F2);
    CHECK_KEY(log, 6, INPUT_KEY_F3);
    CHECK_KEY(log, 7, INPUT_KEY_F4);
}

TEST(ansi_printable_tab_and_control)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "aZ \t~\x03", 0);
    CHECK_EQ(log.n, 6);
    CHECK_CHAR(log, 0, 'a');
    CHECK_CHAR(log, 1, 'Z');
    CHECK_CHAR(log, 2, ' ');
    CHECK_CHAR(log, 3, '\t');
    CHECK_CHAR(log, 4, '~');
    CHECK_CHAR(log, 5, 3);
    CHECK_EQ(log.ev[0].value, 0);
    CHECK_EQ(log.ev[0].source, 0);
}

TEST(ansi_unknown_sequences_swallowed)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    /* Home, End, Insert, F5, a long parameter, SS3 X, mouse-like CSI, Alt-x. */
    feed(&p, &log, "\x1b[H\x1b[F\x1b[2~\x1b[15~\x1b[99999999~\x1bOX\x1b[<0;1;2M\x1bx", 0);
    CHECK_EQ(log.n, 0);
    /* 8-bit bytes and NUL are swallowed too. */
    feed(&p, &log, "\x80\xff", 0);
    feed(&p, &log, "q", 0);
    CHECK_EQ(log.n, 1);
    CHECK_CHAR(log, 0, 'q');
}

TEST(ansi_split_across_calls)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1b", 0);
    poll(&p, &log, 10);
    feed(&p, &log, "[", 20);
    poll(&p, &log, 30);
    feed(&p, &log, "1", 40);
    feed(&p, &log, "2", 41);
    feed(&p, &log, "~", 42);
    feed(&p, &log, "\x1bO", 43);
    feed(&p, &log, "D\r", 44);
    feed(&p, &log, "\n", 45);
    CHECK_EQ(log.n, 3);
    CHECK_KEY(log, 0, INPUT_KEY_F2);
    CHECK_KEY(log, 1, INPUT_KEY_LEFT);
    CHECK_KEY(log, 2, INPUT_KEY_SELECT);
}

TEST(ansi_lone_esc_timeout)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1b", 1000);
    poll(&p, &log, 1000);
    poll(&p, &log, 1049);
    CHECK_EQ(log.n, 0);
    poll(&p, &log, 1050);
    CHECK_EQ(log.n, 1);
    CHECK_KEY(log, 0, INPUT_KEY_BACK);
    /* Only once. */
    poll(&p, &log, 5000);
    CHECK_EQ(log.n, 1);
    /* A byte after the timeout is plain again. */
    feed(&p, &log, "[A", 5001);
    CHECK_EQ(log.n, 3);
    CHECK_CHAR(log, 1, '[');
    CHECK_CHAR(log, 2, 'A');
}

TEST(ansi_esc_timeout_across_wrap)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1b", 0xfffffff0u);
    poll(&p, &log, 0x10);
    CHECK_EQ(log.n, 0);
    poll(&p, &log, 0x22);
    CHECK_EQ(log.n, 1);
}

TEST(ansi_esc_then_control_or_esc)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    /* ESC ESC [A: the first ESC is alone, the second starts a sequence. */
    feed(&p, &log, "\x1b\x1b[A", 0);
    CHECK_EQ(log.n, 2);
    CHECK_KEY(log, 0, INPUT_KEY_BACK);
    CHECK_KEY(log, 1, INPUT_KEY_UP);
    /* ESC CR: BACK then SELECT in one call. */
    feed(&p, &log, "\x1b\r", 0);
    CHECK_EQ(log.n, 4);
    CHECK_KEY(log, 2, INPUT_KEY_BACK);
    CHECK_KEY(log, 3, INPUT_KEY_SELECT);
}

TEST(ansi_unfinished_sequence_dropped)
{
    input_ansi_t p;
    evlog_t log = { .n = 0 };

    input_ansi_init(&p, 50);
    feed(&p, &log, "\x1b[1", 0);
    poll(&p, &log, 100);
    CHECK_EQ(log.n, 0);
    feed(&p, &log, "A", 101);
    CHECK_EQ(log.n, 1);
    CHECK_CHAR(log, 0, 'A');
    /* A control byte inside a sequence aborts it and counts. */
    feed(&p, &log, "\x1b[1\r", 200);
    CHECK_EQ(log.n, 2);
    CHECK_KEY(log, 1, INPUT_KEY_SELECT);
}
