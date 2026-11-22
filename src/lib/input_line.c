#include <stdint.h>

#include <robco/input.h>

/*
 * Line reader and tokenizer of the script channel. Blanks are spaces and
 * tabs. A line ends at CR or LF.
 */

static int blank(char c)
{
    return c == ' ' || c == '\t';
}

void input_line_init(input_line_t *l)
{
    l->len = 0;
    l->overflow = 0;
    l->line = l->buf;
    l->buf[0] = 0;
}

char *input_line_trim(char *s)
{
    char *end;

    while (blank(*s)) {
        s++;
    }
    end = s;
    while (*end) {
        end++;
    }
    while (end > s && (blank(end[-1]) || end[-1] == '\r')) {
        end--;
    }
    *end = 0;
    return s;
}

int input_line_feed(input_line_t *l, char c)
{
    if (c != '\r' && c != '\n') {
        if (l->len < sizeof(l->buf) - 1) {
            l->buf[l->len++] = c;
        } else {
            l->overflow = 1;
        }
        return INPUT_LINE_NONE;
    }
    if (l->overflow) {
        l->len = 0;
        l->overflow = 0;
        return INPUT_LINE_LONG;
    }
    if (l->len == 0) {
        return INPUT_LINE_NONE;
    }
    l->buf[l->len] = 0;
    l->len = 0;
    l->line = input_line_trim(l->buf);
    return *l->line ? INPUT_LINE_READY : INPUT_LINE_NONE;
}

char *input_line_word(char *s)
{
    while (*s && !blank(*s)) {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    while (blank(*s)) {
        s++;
    }
    return s;
}

int input_line_split(char *s, char *argv[], int max)
{
    int argc = 0;

    while (*s) {
        while (blank(*s)) {
            *s++ = 0;
        }
        if (!*s) {
            break;
        }
        if (argc == max) {
            return -1;
        }
        argv[argc++] = s;
        while (*s && !blank(*s)) {
            s++;
        }
    }
    argv[argc] = 0;
    return argc;
}
