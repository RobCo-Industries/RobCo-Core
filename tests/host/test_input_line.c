#include <stdint.h>
#include <string.h>

#include <robco/input.h>

#include "rc_test.h"

/* Feeds bytes and returns the last status that was not INPUT_LINE_NONE. */
static int feed(input_line_t *l, const char *bytes)
{
    int last = INPUT_LINE_NONE;

    for (; *bytes; bytes++) {
        int r = input_line_feed(l, *bytes);

        if (r != INPUT_LINE_NONE) {
            last = r;
        }
    }
    return last;
}

static int split(const char *text, char *argv[], int max)
{
    static char buf[INPUT_SCRIPT_LINE_MAX];

    strcpy(buf, text);
    return input_line_split(input_line_trim(buf), argv, max);
}

TEST(line_trims_blanks_and_cr)
{
    input_line_t l;

    input_line_init(&l);
    CHECK_EQ(feed(&l, " \t board \t\r\n"), INPUT_LINE_READY);
    CHECK_STR(l.line, "board");
    CHECK_EQ(feed(&l, "  key up\n"), INPUT_LINE_READY);
    CHECK_STR(l.line, "key up");
    CHECK_EQ(feed(&l, "text a  b\r"), INPUT_LINE_READY);
    CHECK_STR(l.line, "text a  b");
}

TEST(line_blank_gives_nothing)
{
    input_line_t l;

    input_line_init(&l);
    CHECK_EQ(input_line_feed(&l, '\n'), INPUT_LINE_NONE);
    CHECK_EQ(feed(&l, " \t \r\n"), INPUT_LINE_NONE);
    CHECK_EQ(feed(&l, "\r\n\r\n"), INPUT_LINE_NONE);
}

TEST(line_too_long_once_then_resyncs)
{
    input_line_t l;
    char big[INPUT_SCRIPT_LINE_MAX * 3];
    unsigned i;
    int longs = 0;

    input_line_init(&l);
    /* The longest line that fits. */
    memset(big, 'a', INPUT_SCRIPT_LINE_MAX - 1);
    big[INPUT_SCRIPT_LINE_MAX - 1] = 0;
    CHECK_EQ(feed(&l, big), INPUT_LINE_NONE);
    CHECK_EQ(input_line_feed(&l, '\n'), INPUT_LINE_READY);
    CHECK_EQ(strlen(l.line), INPUT_SCRIPT_LINE_MAX - 1);
    /* One byte more, then far more: one LONG each, at the end. */
    memset(big, 'b', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    for (i = 0; big[i]; i++) {
        CHECK_EQ(input_line_feed(&l, big[i]), INPUT_LINE_NONE);
    }
    CHECK_EQ(input_line_feed(&l, '\r'), INPUT_LINE_LONG);
    CHECK_EQ(input_line_feed(&l, '\n'), INPUT_LINE_NONE);
    for (i = 0; i < INPUT_SCRIPT_LINE_MAX; i++) {
        input_line_feed(&l, 'c');
    }
    for (i = 0; i < 3; i++) {
        longs += input_line_feed(&l, '\n') == INPUT_LINE_LONG;
    }
    CHECK_EQ(longs, 1);
    CHECK_EQ(feed(&l, "stats\n"), INPUT_LINE_READY);
    CHECK_STR(l.line, "stats");
}

TEST(word_splits_at_blank_runs)
{
    char buf[32];
    char *rest;

    strcpy(buf, "key\t \tup 1");
    rest = input_line_word(buf);
    CHECK_STR(buf, "key");
    CHECK_STR(rest, "up 1");
    strcpy(buf, "stats");
    rest = input_line_word(buf);
    CHECK_STR(buf, "stats");
    CHECK_STR(rest, "");
}

TEST(split_words)
{
    char *argv[INPUT_SCRIPT_ARGS_MAX + 1];

    CHECK_EQ(split("  board  ", argv, INPUT_SCRIPT_ARGS_MAX), 1);
    CHECK_STR(argv[0], "board");
    CHECK(argv[1] == NULL);
    CHECK_EQ(split("a\t\tb  \t c", argv, INPUT_SCRIPT_ARGS_MAX), 3);
    CHECK_STR(argv[0], "a");
    CHECK_STR(argv[1], "b");
    CHECK_STR(argv[2], "c");
    CHECK(argv[3] == NULL);
    CHECK_EQ(split("", argv, INPUT_SCRIPT_ARGS_MAX), 0);
    CHECK(argv[0] == NULL);
    CHECK_EQ(split(" \t ", argv, INPUT_SCRIPT_ARGS_MAX), 0);
}

TEST(split_word_limit)
{
    char *argv[INPUT_SCRIPT_ARGS_MAX + 1];

    CHECK_EQ(split("1 2 3 4 5 6 7 8", argv, INPUT_SCRIPT_ARGS_MAX), 8);
    CHECK_STR(argv[7], "8");
    CHECK(argv[8] == NULL);
    CHECK_EQ(split("1 2 3 4 5 6 7 8 9", argv, INPUT_SCRIPT_ARGS_MAX), -1);
    CHECK_EQ(split("1 2 3 4 5 6 7 8  ", argv, INPUT_SCRIPT_ARGS_MAX), 8);
}

TEST(split_long_word)
{
    char *argv[INPUT_SCRIPT_ARGS_MAX + 1];
    char word[INPUT_SCRIPT_LINE_MAX];

    memset(word, 'w', sizeof(word) - 1);
    word[sizeof(word) - 1] = 0;
    CHECK_EQ(split(word, argv, INPUT_SCRIPT_ARGS_MAX), 1);
    CHECK_EQ(strlen(argv[0]), INPUT_SCRIPT_LINE_MAX - 1);
}
