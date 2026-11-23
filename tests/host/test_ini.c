#include <stdint.h>
#include <stdlib.h>

#include <robco/ini.h>

#include "rc_test.h"

#define MAX_SEEN 16

typedef struct {
    int count;
    int stop_at;
    char text[MAX_SEEN][256];
    unsigned line[MAX_SEEN];
} seen_t;

static int record(const char *section, const char *key, const char *value,
                  unsigned line, void *ctx)
{
    seen_t *s = ctx;

    if (s->count < MAX_SEEN) {
        snprintf(s->text[s->count], sizeof(s->text[0]), "%s|%s|%s", section, key, value);
        s->line[s->count] = line;
    }
    s->count++;
    return s->stop_at != 0 && s->count == s->stop_at;
}

static int parse(const char *text, seen_t *s, unsigned *line)
{
    memset(s, 0, sizeof(*s));
    return ini_parse(text, strlen(text), record, s, line);
}

/* Expects `text` to fail with `code` at `line`. */
static void expect_error(const char *text, int code, unsigned line)
{
    seen_t s;
    unsigned got = 99;
    int rc = parse(text, &s, &got);

    if (rc != code || got != line) {
        RC_FAIL("\"%s\": rc %d line %u, expected %d line %u", text, rc, got, code, line);
    }
}

TEST(basic)
{
    seen_t s;
    unsigned line;
    const char *t =
        "top = 1\n"
        "# whole-line comment\n"
        "; other comment\n"
        "\n"
        "[video]\n"
        "  width=640  \n"
        "\theight\t=\t480\t; trailing\n"
        "[ audio ]   # comment after a section\n"
        "volume = 7 # trailing\n"
        "empty =\n";

    CHECK_EQ(parse(t, &s, &line), 0);
    CHECK_EQ(line, 0);
    CHECK_EQ(s.count, 5);
    CHECK_STR(s.text[0], "|top|1");
    CHECK_EQ(s.line[0], 1);
    CHECK_STR(s.text[1], "video|width|640");
    CHECK_EQ(s.line[1], 6);
    CHECK_STR(s.text[2], "video|height|480");
    CHECK_STR(s.text[3], "audio|volume|7");
    CHECK_STR(s.text[4], "audio|empty|");
    CHECK_EQ(s.line[4], 10);
}

TEST(comment_rule)
{
    seen_t s;

    CHECK_EQ(parse("a = b#c\nb = x ;y\nc = #all comment\nd = \"#kept\" # gone\ne = p;q\n",
                   &s, NULL), 0);
    CHECK_EQ(s.count, 5);
    CHECK_STR(s.text[0], "|a|b#c");
    CHECK_STR(s.text[1], "|b|x");
    CHECK_STR(s.text[2], "|c|");
    CHECK_STR(s.text[3], "|d|#kept");
    CHECK_STR(s.text[4], "|e|p;q");
}

TEST(quoted)
{
    seen_t s;

    CHECK_EQ(parse("a = \"  spaced  \"\nb = \"q\\\"x\\\\y\\nz\"\nc = \"\"\nd=\"a = b ; c\"\n",
                   &s, NULL), 0);
    CHECK_EQ(s.count, 4);
    CHECK_STR(s.text[0], "|a|  spaced  ");
    CHECK_STR(s.text[1], "|b|q\"x\\y\nz");
    CHECK_STR(s.text[2], "|c|");
    CHECK_STR(s.text[3], "|d|a = b ; c");
}

TEST(line_ends)
{
    seen_t s;
    unsigned line;

    CHECK_EQ(parse("[s]\r\na = 1\r\n\r\nb = 2", &s, &line), 0);
    CHECK_EQ(s.count, 2);
    CHECK_STR(s.text[0], "s|a|1");
    CHECK_STR(s.text[1], "s|b|2");
    CHECK_EQ(s.line[1], 4);
    CHECK_EQ(parse("a = \"x\"\r\nb = y \r\n", &s, NULL), 0);
    CHECK_STR(s.text[1], "|b|y");
    CHECK_EQ(parse("", &s, NULL), 0);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(ini_parse(NULL, 0, record, &s, NULL), 0);
}

TEST(syntax_errors)
{
    expect_error("a = 1\nnovalue\n", INI_ERR_SYNTAX, 2);
    expect_error("= 1\n", INI_ERR_SYNTAX, 1);
    expect_error("a b = 1\n", INI_ERR_SYNTAX, 1);
    expect_error("\n\n[open\n", INI_ERR_SYNTAX, 3);
    expect_error("[]\n", INI_ERR_SYNTAX, 1);
    expect_error("[a$b]\n", INI_ERR_SYNTAX, 1);
    expect_error("[ \t ]\n", INI_ERR_SYNTAX, 1);
    expect_error("[a]b]\n", INI_ERR_SYNTAX, 1);
    expect_error("[a[b]\n", INI_ERR_SYNTAX, 1);
    expect_error("[a b\n", INI_ERR_SYNTAX, 1);
    expect_error("[a] x\n", INI_ERR_SYNTAX, 1);
    expect_error("a = \"open\n", INI_ERR_SYNTAX, 1);
    expect_error("a = \"x\" y\n", INI_ERR_SYNTAX, 1);
    expect_error("a = \"bad \\t escape\"\n", INI_ERR_SYNTAX, 1);
    expect_error("a = \"ends in \\", INI_ERR_SYNTAX, 1);
    expect_error("k$y = 1\n", INI_ERR_SYNTAX, 1);
    expect_error("x=1\ny=2\nz=3\n[ok]\n!\n", INI_ERR_SYNTAX, 5);
}

TEST(char_errors)
{
    static const char nul[] = "a = 1\nb = x\0y\n";
    seen_t s;
    unsigned line;

    memset(&s, 0, sizeof(s));
    CHECK_EQ(ini_parse(nul, sizeof(nul) - 1, record, &s, &line), INI_ERR_CHAR);
    CHECK_EQ(line, 2);
    CHECK_EQ(s.count, 1);
    expect_error("a = 1\rb = 2\n", INI_ERR_CHAR, 1);
    expect_error("a = 1\n\x01\n", INI_ERR_CHAR, 2);
    expect_error("a = \x7f\n", INI_ERR_CHAR, 1);
    expect_error("a = \"\x1b[0m\"\n", INI_ERR_CHAR, 1);
}

TEST(utf8_value)
{
    seen_t s;

    CHECK_EQ(parse("name = Vault-Tec \xc2\xae\n", &s, NULL), 0);
    CHECK_STR(s.text[0], "|name|Vault-Tec \xc2\xae");
}

TEST(limits)
{
    char buf[600];
    seen_t s;

    /* Longest key, section and value pass. */
    snprintf(buf, sizeof(buf), "[%.*s]\n%.*s = %.*s\n",
             INI_MAX_SECTION, "ssssssssssssssssssssssssssssssssssssssss",
             INI_MAX_KEY, "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk",
             INI_MAX_VALUE, "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv"
                            "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv");
    CHECK_EQ(parse(buf, &s, NULL), 0);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(strlen(s.text[0]), INI_MAX_SECTION + INI_MAX_KEY + INI_MAX_VALUE + 2);

    /* One more byte fails with the line number. */
    snprintf(buf, sizeof(buf), "a=1\n[%.*s]\n", INI_MAX_SECTION + 1,
             "ssssssssssssssssssssssssssssssssssssssss");
    expect_error(buf, INI_ERR_TOO_LONG, 2);
    snprintf(buf, sizeof(buf), "\n\n%.*s = 1\n", INI_MAX_KEY + 1,
             "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk");
    expect_error(buf, INI_ERR_TOO_LONG, 3);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "v = ", 4);
    memset(buf + 4, 'x', INI_MAX_VALUE + 1);
    expect_error(buf, INI_ERR_TOO_LONG, 1);

    /* Quoted value: the limit counts bytes after escapes. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "v = \"", 5);
    memset(buf + 5, 'x', INI_MAX_VALUE - 60);
    for (int i = 0; i < 60; i++) {
        memcpy(buf + 5 + (INI_MAX_VALUE - 60) + 2 * i, "\\n", 2);
    }
    buf[5 + INI_MAX_VALUE + 60] = '"';
    CHECK_EQ(parse(buf, &s, NULL), 0);
    CHECK_EQ(strlen(s.text[0]), INI_MAX_VALUE + 3);
    buf[5 + INI_MAX_VALUE + 60] = 'x';
    buf[6 + INI_MAX_VALUE + 60] = '"';
    expect_error(buf, INI_ERR_TOO_LONG, 1);

    /* A long comment line is still a long line. */
    memset(buf, 0, sizeof(buf));
    buf[0] = '#';
    memset(buf + 1, 'c', INI_MAX_LINE - 1);
    CHECK_EQ(parse(buf, &s, NULL), 0);
    buf[INI_MAX_LINE] = 'c';
    expect_error(buf, INI_ERR_TOO_LONG, 1);
    /* The CR of CR LF does not count. */
    buf[INI_MAX_LINE] = '\r';
    buf[INI_MAX_LINE + 1] = '\n';
    CHECK_EQ(parse(buf, &s, NULL), 0);
}

TEST(handler_stops)
{
    seen_t s;

    memset(&s, 0, sizeof(s));
    s.stop_at = 2;
    CHECK_EQ(ini_parse("a=1\nb=2\nc=3\n", 12, record, &s, NULL), INI_STOPPED);
    CHECK_EQ(s.count, 2);
}

TEST(bad_args)
{
    seen_t s;
    char out[8];
    int32_t v;
    int b;
    unsigned line = 5;

    CHECK_EQ(ini_parse(rc_launder(NULL), 3, record, &s, &line), INI_ERR_ARG);
    CHECK_EQ(line, 0);
    CHECK_EQ(ini_parse("a=1", 3, rc_launder(NULL), &s, NULL), INI_ERR_ARG);
    CHECK_EQ(ini_get("a=1", 3, "", "a", out, 0), INI_ERR_ARG);
    CHECK_EQ(ini_get("a=1", 3, rc_launder(NULL), "a", out, sizeof(out)), INI_ERR_ARG);
    CHECK_EQ(ini_get_int("a=1", 3, "", "a", 0, 1, rc_launder(NULL)), INI_ERR_ARG);
    CHECK_EQ(ini_get_bool("a=1", 3, "", "a", rc_launder(NULL)), INI_ERR_ARG);
    (void)v;
    (void)b;
}

static const char CONF[] =
    "name = first\n"
    "[display]\n"
    "width = 640\n"
    "hex = 0x1F\n"
    "neg = -12\n"
    "big = 4294967296\n"
    "max = 2147483647\n"
    "min = -2147483648\n"
    "word = 12ab\n"
    "on = YES\n"
    "off = Off\n"
    "maybe = perhaps\n"
    "width = 800\n"
    "[other]\n"
    "width = 1\n"
    "name = second\n";

#define GET(sec, key, out) ini_get(CONF, sizeof(CONF) - 1, sec, key, out, sizeof(out))

TEST(get)
{
    char out[16];
    char tiny[4];
    seen_t s;

    CHECK_EQ(GET("", "name", out), 0);
    CHECK_STR(out, "first");
    CHECK_EQ(GET("other", "name", out), 0);
    CHECK_STR(out, "second");
    /* Duplicate key: last one wins, the callback sees both. */
    CHECK_EQ(GET("display", "width", out), 0);
    CHECK_STR(out, "800");
    memset(&s, 0, sizeof(s));
    CHECK_EQ(ini_parse(CONF, sizeof(CONF) - 1, record, &s, NULL), 0);
    CHECK_STR(s.text[1], "display|width|640");
    CHECK_STR(s.text[11], "display|width|800");
    CHECK_EQ(GET("display", "nope", out), INI_ERR_NOT_FOUND);
    CHECK_STR(out, "");
    CHECK_EQ(GET("nope", "width", out), INI_ERR_NOT_FOUND);
    CHECK_EQ(GET("display", "Width", out), INI_ERR_NOT_FOUND);
    CHECK_EQ(GET("display", "width", tiny), 0);
    CHECK_EQ(GET("display", "maybe", tiny), INI_ERR_SPACE);
    CHECK_STR(tiny, "");
    /* A parse error anywhere fails the lookup. */
    CHECK_EQ(ini_get("a=1\nbad\n", 8, "", "a", out, sizeof(out)), INI_ERR_SYNTAX);
}

TEST(get_int)
{
    const size_t n = sizeof(CONF) - 1;
    int32_t v = 0;

    CHECK_EQ(ini_get_int(CONF, n, "display", "width", 0, 1000, &v), 0);
    CHECK_EQ(v, 800);
    CHECK_EQ(ini_get_int(CONF, n, "display", "hex", 0, 1000, &v), 0);
    CHECK_EQ(v, 31);
    CHECK_EQ(ini_get_int(CONF, n, "display", "neg", -100, 0, &v), 0);
    CHECK_EQ((int64_t)v, -12);
    CHECK_EQ(ini_get_int(CONF, n, "display", "max", INT32_MIN, INT32_MAX, &v), 0);
    CHECK_EQ(v, INT32_MAX);
    CHECK_EQ(ini_get_int(CONF, n, "display", "min", INT32_MIN, INT32_MAX, &v), 0);
    CHECK_EQ((int64_t)v, INT32_MIN);
    v = 7;
    CHECK_EQ(ini_get_int(CONF, n, "display", "width", 0, 799, &v), INI_ERR_RANGE);
    CHECK_EQ(v, 7);
    CHECK_EQ(ini_get_int(CONF, n, "display", "neg", 0, 100, &v), INI_ERR_RANGE);
    CHECK_EQ(ini_get_int(CONF, n, "display", "big", INT32_MIN, INT32_MAX, &v), INI_ERR_RANGE);
    CHECK_EQ(ini_get_int(CONF, n, "display", "word", INT32_MIN, INT32_MAX, &v), INI_ERR_VALUE);
    CHECK_EQ(ini_get_int(CONF, n, "display", "on", INT32_MIN, INT32_MAX, &v), INI_ERR_VALUE);
    CHECK_EQ(ini_get_int(CONF, n, "display", "nope", 0, 1, &v), INI_ERR_NOT_FOUND);
    CHECK_EQ(ini_get_int("a=0x\n", 5, "", "a", 0, 1, &v), INI_ERR_VALUE);
    CHECK_EQ(ini_get_int("a=-\n", 4, "", "a", 0, 1, &v), INI_ERR_VALUE);
    CHECK_EQ(ini_get_int("a=\n", 3, "", "a", 0, 1, &v), INI_ERR_VALUE);
    CHECK_EQ(ini_get_int("a=-0x1\n", 7, "", "a", -5, 5, &v), INI_ERR_VALUE);
    CHECK_EQ(ini_get_int("a=0xFFFFFFFF\n", 13, "", "a", INT32_MIN, INT32_MAX, &v), INI_ERR_RANGE);
    CHECK_EQ(ini_get_int("a=99999999999999999999\n", 23, "", "a", INT32_MIN, INT32_MAX, &v),
             INI_ERR_RANGE);
}

TEST(get_bool)
{
    static const char *const yes[] = { "true", "TRUE", "Yes", "on", "1" };
    static const char *const no[] = { "false", "No", "OFF", "0" };
    static const char *const bad[] = { "", "2", "tru", "truee", "y", "enabled" };
    char buf[64];
    int b;
    unsigned i;

    for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        snprintf(buf, sizeof(buf), "k = %s\n", yes[i]);
        b = -1;
        CHECK_EQ(ini_get_bool(buf, strlen(buf), "", "k", &b), 0);
        CHECK_EQ(b, 1);
    }
    for (i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        snprintf(buf, sizeof(buf), "k = %s\n", no[i]);
        b = -1;
        CHECK_EQ(ini_get_bool(buf, strlen(buf), "", "k", &b), 0);
        CHECK_EQ(b, 0);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(buf, sizeof(buf), "k = %s\n", bad[i]);
        CHECK_EQ(ini_get_bool(buf, strlen(buf), "", "k", &b), INI_ERR_VALUE);
    }
}

static int count_only(const char *section, const char *key, const char *value,
                      unsigned line, void *ctx)
{
    (void)section;
    (void)key;
    (void)value;
    (void)line;
    (*(unsigned *)ctx)++;
    return 0;
}

TEST(binary_garbage)
{
    unsigned char *buf = malloc(4096);
    unsigned seed = 12345;
    unsigned count;
    unsigned line;
    int rc;
    int i;
    int round;
    int bad = 0;

    for (round = 0; round < 200; round++) {
        for (i = 0; i < 4096; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = (unsigned char)(seed >> 16);
        }
        count = 0;
        rc = ini_parse((const char *)buf, 4096, count_only, &count, &line);
        if (rc >= 0 || line == 0) {
            bad++;
        }
    }
    CHECK_EQ(bad, 0);
    free(buf);
}

TEST(large_input)
{
    const size_t size = 64 * 1024;
    char *buf = malloc(size);
    size_t n = 0;
    unsigned count = 0;
    unsigned line;
    unsigned keys = 0;
    char out[32];

    n += (size_t)snprintf(buf + n, size - n, "[big]\n");
    while (size - n > 32) {
        n += (size_t)snprintf(buf + n, size - n, "key%u = value %u\n", keys, keys);
        keys++;
    }
    CHECK_EQ(ini_parse(buf, n, count_only, &count, &line), 0);
    CHECK_EQ(count, keys);
    CHECK(n > 60 * 1024);
    CHECK_EQ(ini_get(buf, n, "big", "key100", out, sizeof(out)), 0);
    CHECK_STR(out, "value 100");
    free(buf);
}

TEST(strerror)
{
    CHECK_STR(ini_strerror(0), "ok");
    CHECK_STR(ini_strerror(INI_ERR_TOO_LONG), "line, name or value too long");
    CHECK_STR(ini_strerror(-100), "unknown error");
}

TEST(section_names_with_spaces)
{
    char buf[128];
    seen_t s;

    CHECK_EQ(parse("[entry UOS]\na = 1\n[ \tentry  two\t ] ; c\nb = 2\n[x\ty]\nc = 3\n", &s,
                   NULL), 0);
    CHECK_EQ(s.count, 3);
    CHECK_STR(s.text[0], "entry UOS|a|1");
    CHECK_STR(s.text[1], "entry  two|b|2");
    CHECK_STR(s.text[2], "x\ty|c|3");

    /* The limit counts inner spaces but not the ones trimmed. */
    snprintf(buf, sizeof(buf), "[   %.*s %.*s   ]\nk = v\n", INI_MAX_SECTION / 2,
             "ssssssssssssssssssssssssssssssss", INI_MAX_SECTION - INI_MAX_SECTION / 2 - 1,
             "ssssssssssssssssssssssssssssssss");
    CHECK_EQ(parse(buf, &s, NULL), 0);
    CHECK_EQ(strlen(s.text[0]), INI_MAX_SECTION + 4);
    snprintf(buf, sizeof(buf), "[%.*s %.*s]\n", INI_MAX_SECTION / 2,
             "ssssssssssssssssssssssssssssssss", INI_MAX_SECTION - INI_MAX_SECTION / 2,
             "ssssssssssssssssssssssssssssssss");
    expect_error(buf, INI_ERR_TOO_LONG, 1);
}

/* The BOOT.CFG example from docs/spec/BOOT_PROTOCOL.md, read from the file. */
TEST(boot_protocol_example)
{
    static char doc[65536];
    char val[INI_MAX_VALUE + 1];
    FILE *f = fopen("docs/spec/BOOT_PROTOCOL.md", "rb");
    size_t n = 0;
    const char *start;
    const char *end;
    int32_t timeout = 0;
    seen_t s;

    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    n = fread(doc, 1, sizeof(doc) - 1, f);
    fclose(f);
    doc[n] = '\0';
    start = strstr(doc, "### BOOT.CFG");
    CHECK(start != NULL);
    if (start == NULL) {
        return;
    }
    start = strstr(start, "```\n");
    CHECK(start != NULL);
    if (start == NULL) {
        return;
    }
    start += 4;
    end = strstr(start, "```");
    CHECK(end != NULL);
    if (end == NULL) {
        return;
    }
    memset(&s, 0, sizeof(s));
    CHECK_EQ(ini_parse(start, (size_t)(end - start), record, &s, NULL), 0);
    CHECK_EQ(s.count, 6);
    CHECK_EQ(ini_get(start, (size_t)(end - start), "boot", "default", val, sizeof(val)), 0);
    CHECK_STR(val, "UOS");
    CHECK_EQ(ini_get_int(start, (size_t)(end - start), "boot", "timeout", 0, 60, &timeout), 0);
    CHECK_EQ(timeout, 3);
    CHECK_EQ(ini_get(start, (size_t)(end - start), "entry UOS", "path", val, sizeof(val)), 0);
    CHECK_STR(val, "/ROBCO/UOS.SYS");
    CHECK_EQ(ini_get(start, (size_t)(end - start), "entry UOS", "cmdline", val, sizeof(val)), 0);
    CHECK_STR(val, "");
    CHECK_EQ(ini_get(start, (size_t)(end - start), "entry MAINT", "path", val, sizeof(val)), 0);
    CHECK_STR(val, "/ROBCO/MAINT.SYS");
    CHECK_EQ(ini_get(start, (size_t)(end - start), "entry DIAG", "path", val, sizeof(val)), 0);
    CHECK_STR(val, "/ROBCO/DIAG.SYS");
}

/* Section headers and keys in one list, as "[name]@line" and "section|key|value". */
static int record_section(const char *section, unsigned line, void *ctx)
{
    seen_t *s = ctx;

    if (s->count < MAX_SEEN) {
        snprintf(s->text[s->count], sizeof(s->text[0]), "[%s]@%u", section, line);
        s->line[s->count] = line;
    }
    s->count++;
    return s->stop_at != 0 && s->count == s->stop_at;
}

static int parse_both(const char *text, seen_t *s, unsigned *line)
{
    memset(s, 0, sizeof(*s));
    return ini_parse_sections(text, strlen(text), record, record_section, s, line);
}

TEST(sections_in_file_order_with_keys)
{
    seen_t s;

    CHECK_EQ(parse_both("top = 1\n[a]\nx = 2\n\n[ b ]  # note\n[a]\ny = 3\n", &s, NULL), 0);
    CHECK_EQ(s.count, 6);
    CHECK_STR(s.text[0], "|top|1");
    CHECK_STR(s.text[1], "[a]@2");
    CHECK_STR(s.text[2], "a|x|2");
    /* Empty, and a repeated name: each header is reported. */
    CHECK_STR(s.text[3], "[b]@5");
    CHECK_STR(s.text[4], "[a]@6");
    CHECK_STR(s.text[5], "a|y|3");
}

TEST(sections_only_and_stop)
{
    seen_t s;
    unsigned line = 99;

    memset(&s, 0, sizeof(s));
    CHECK_EQ(ini_parse_sections("[a]\nk=v\n[b]\n[c]\n", 16, NULL, record_section, &s, &line), 0);
    CHECK_EQ(line, 0);
    CHECK_EQ(s.count, 3);
    CHECK_STR(s.text[2], "[c]@4");

    memset(&s, 0, sizeof(s));
    s.stop_at = 2;
    CHECK_EQ(ini_parse_sections("[a]\n[b]\n[c]\n", 12, NULL, record_section, &s, NULL),
             INI_STOPPED);
    CHECK_EQ(s.count, 2);

    CHECK_EQ(ini_parse_sections("[a]\n", 4, NULL, NULL, &s, &line), INI_ERR_ARG);
    CHECK_EQ(line, 0);
    CHECK_EQ(ini_parse_sections(rc_launder(NULL), 3, record, record_section, &s, NULL),
             INI_ERR_ARG);
    CHECK_EQ(ini_parse_sections(NULL, 0, NULL, record_section, &s, NULL), 0);

    /* Headers before a bad line are reported; the error still comes back. */
    CHECK_EQ(parse_both("[a]\n[b\n", &s, &line), INI_ERR_SYNTAX);
    CHECK_EQ(line, 2);
    CHECK_EQ(s.count, 1);
}

/* The BOOT.CFG example from docs/spec/BOOT_PROTOCOL.md: the entry headers, in order. */
TEST(boot_protocol_example_sections)
{
    static char doc[65536];
    FILE *f = fopen("docs/spec/BOOT_PROTOCOL.md", "rb");
    const char *start;
    const char *end;
    size_t n;
    seen_t s;
    int i, k = 0;
    static const char *const want[] = { "[boot]", "[entry UOS]", "[entry MAINT]",
                                        "[entry DIAG]" };

    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    n = fread(doc, 1, sizeof(doc) - 1, f);
    fclose(f);
    doc[n] = '\0';
    start = strstr(doc, "### BOOT.CFG");
    start = start != NULL ? strstr(start, "```\n") : NULL;
    end = start != NULL ? strstr(start + 4, "```") : NULL;
    CHECK(end != NULL);
    if (end == NULL) {
        return;
    }
    start += 4;
    memset(&s, 0, sizeof(s));
    CHECK_EQ(ini_parse_sections(start, (size_t)(end - start), NULL, record_section, &s, NULL), 0);
    CHECK_EQ(s.count, 4);
    for (i = 0; i < s.count && i < 4; i++) {
        /* "[name]@line": compare the name part. */
        CHECK(strncmp(s.text[i], want[i], strlen(want[i])) == 0);
        CHECK_EQ(s.text[i][strlen(want[i])], '@');
        k++;
    }
    CHECK_EQ(k, 4);
}
