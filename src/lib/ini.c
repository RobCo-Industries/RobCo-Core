#include <stddef.h>
#include <stdint.h>

#include <robco/ini.h>

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_' || c == '.' || c == '-';
}

static int is_comment(char c)
{
    return c == '#' || c == ';';
}

/* Moves *p past spaces and tabs, up to `end`. */
static void skip_space(const char **p, const char *end)
{
    while (*p < end && is_space(**p)) {
        (*p)++;
    }
}

/* True if only spaces, tabs and an optional comment remain. */
static int rest_is_blank(const char *p, const char *end)
{
    skip_space(&p, end);
    return p == end || is_comment(*p);
}

/*
 * Copies a name of [A-Za-z0-9_.-] starting at *p into `out` (max chars,
 * plus NUL) and moves *p past it. Returns 0, or an error for an empty or
 * long name.
 */
static int take_name(const char **p, const char *end, char *out, size_t max)
{
    size_t n = 0;

    while (*p < end && is_name_char(**p)) {
        if (n == max) {
            return INI_ERR_TOO_LONG;
        }
        out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
    return n == 0 ? INI_ERR_SYNTAX : 0;
}

/*
 * Section name between '[' and ']': name characters, with spaces and tabs
 * inside. Leading and trailing ones are dropped and do not count toward
 * the limit. `p` points after '['. Returns 0 or an error.
 */
static int take_section(const char *p, const char *end, char *out)
{
    const char *start;
    const char *stop;
    size_t n = 0;

    skip_space(&p, end);
    start = p;
    while (p < end && *p != ']') {
        if (!is_name_char(*p) && !is_space(*p)) {
            return INI_ERR_SYNTAX;
        }
        p++;
    }
    if (p == end) {
        return INI_ERR_SYNTAX;
    }
    stop = p;
    while (stop > start && is_space(stop[-1])) {
        stop--;
    }
    if (stop == start) {
        return INI_ERR_SYNTAX;
    }
    if ((size_t)(stop - start) > INI_MAX_SECTION) {
        return INI_ERR_TOO_LONG;
    }
    while (start < stop) {
        out[n++] = *start++;
    }
    out[n] = '\0';
    return rest_is_blank(p + 1, end) ? 0 : INI_ERR_SYNTAX;
}

static int parse_quoted(const char *p, const char *end, char *out)
{
    size_t n = 0;
    char c;

    p++;    /* opening quote */
    for (;;) {
        if (p == end) {
            return INI_ERR_SYNTAX;  /* no closing quote */
        }
        c = *p++;
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            if (p == end) {
                return INI_ERR_SYNTAX;
            }
            c = *p++;
            if (c == 'n') {
                c = '\n';
            } else if (c != '"' && c != '\\') {
                return INI_ERR_SYNTAX;
            }
        }
        if (n == INI_MAX_VALUE) {
            return INI_ERR_TOO_LONG;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return rest_is_blank(p, end) ? 0 : INI_ERR_SYNTAX;
}

static int parse_plain(const char *p, const char *end, char *out)
{
    const char *start = p;
    const char *stop = p;
    size_t n;

    while (p < end) {
        if (is_comment(*p) && (p == start || is_space(p[-1]))) {
            break;
        }
        p++;
        if (!is_space(p[-1])) {
            stop = p;
        }
    }
    n = (size_t)(stop - start);
    if (n > INI_MAX_VALUE) {
        return INI_ERR_TOO_LONG;
    }
    for (p = start; p < stop; p++) {
        *out++ = *p;
    }
    *out = '\0';
    return 0;
}

typedef struct {
    char section[INI_MAX_SECTION + 1];
    char key[INI_MAX_KEY + 1];
    char value[INI_MAX_VALUE + 1];
} scratch_t;

/* Returns 0 for a blank line, 1 for a key, 2 for a section, or an error. */
static int parse_line(const char *p, const char *end, scratch_t *s)
{
    int rc;

    skip_space(&p, end);
    if (p == end || is_comment(*p)) {
        return 0;
    }
    if (*p == '[') {
        rc = take_section(p + 1, end, s->section);
        return rc < 0 ? rc : 2;
    }
    rc = take_name(&p, end, s->key, INI_MAX_KEY);
    if (rc < 0) {
        return rc;
    }
    skip_space(&p, end);
    if (p == end || *p != '=') {
        return INI_ERR_SYNTAX;
    }
    p++;
    skip_space(&p, end);
    rc = (p < end && *p == '"') ? parse_quoted(p, end, s->value) : parse_plain(p, end, s->value);
    return rc < 0 ? rc : 1;
}

int ini_parse(const char *text, size_t len, ini_handler_t fn, void *ctx, unsigned *err_line)
{
    if (fn == NULL) {
        if (err_line != NULL) {
            *err_line = 0;
        }
        return INI_ERR_ARG;
    }
    return ini_parse_sections(text, len, fn, NULL, ctx, err_line);
}

int ini_parse_sections(const char *text, size_t len, ini_handler_t fn,
                       ini_section_handler_t on_section, void *ctx, unsigned *err_line)
{
    scratch_t s;
    const char *p;
    const char *end;
    const char *eol;
    const char *q;
    unsigned line = 0;
    int rc;

    if (err_line != NULL) {
        *err_line = 0;
    }
    if ((fn == NULL && on_section == NULL) || (text == NULL && len != 0)) {
        return INI_ERR_ARG;
    }
    if (len == 0) {
        return 0;
    }
    s.section[0] = '\0';
    p = text;
    end = text + len;
    while (p < end) {
        line++;
        rc = 0;
        eol = p;
        while (eol < end && *eol != '\n') {
            eol++;
        }
        q = eol;
        if (q > p && q[-1] == '\r') {
            q--;
        }
        if ((size_t)(q - p) > INI_MAX_LINE) {
            rc = INI_ERR_TOO_LONG;
        }
        for (const char *c = p; rc == 0 && c < q; c++) {
            unsigned char b = (unsigned char)*c;

            if ((b < 0x20 && b != '\t') || b == 0x7f) {
                rc = INI_ERR_CHAR;
            }
        }
        if (rc == 0) {
            rc = parse_line(p, q, &s);
        }
        if (rc < 0) {
            if (err_line != NULL) {
                *err_line = line;
            }
            return rc;
        }
        if (rc == 1 && fn != NULL && fn(s.section, s.key, s.value, line, ctx) != 0) {
            return INI_STOPPED;
        }
        if (rc == 2 && on_section != NULL && on_section(s.section, line, ctx) != 0) {
            return INI_STOPPED;
        }
        p = eol < end ? eol + 1 : end;
    }
    return 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

typedef struct {
    const char *section;
    const char *key;
    char *out;
    size_t out_size;
    int rc;
} get_ctx_t;

static int get_handler(const char *section, const char *key, const char *value,
                       unsigned line, void *ctx)
{
    get_ctx_t *g = ctx;
    size_t n = 0;

    (void)line;
    if (!str_eq(section, g->section) || !str_eq(key, g->key)) {
        return 0;
    }
    while (value[n] != '\0') {
        n++;
    }
    if (n >= g->out_size) {
        g->rc = INI_ERR_SPACE;
        g->out[0] = '\0';
        return 0;
    }
    for (size_t i = 0; i <= n; i++) {
        g->out[i] = value[i];
    }
    g->rc = 0;
    return 0;
}

int ini_get(const char *text, size_t len, const char *section, const char *key,
            char *out, size_t out_size)
{
    get_ctx_t g;
    int rc;

    if (section == NULL || key == NULL || out == NULL || out_size == 0) {
        return INI_ERR_ARG;
    }
    out[0] = '\0';
    g.section = section;
    g.key = key;
    g.out = out;
    g.out_size = out_size;
    g.rc = INI_ERR_NOT_FOUND;
    rc = ini_parse(text, len, get_handler, &g, NULL);
    if (rc < 0) {
        out[0] = '\0';
        return rc;
    }
    return g.rc;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int ini_get_int(const char *text, size_t len, const char *section, const char *key,
                int32_t min, int32_t max, int32_t *out)
{
    char buf[INI_MAX_VALUE + 1];
    const char *p = buf;
    int64_t v = 0;
    int neg = 0;
    unsigned base = 10;
    int d;
    int rc;

    if (out == NULL) {
        return INI_ERR_ARG;
    }
    rc = ini_get(text, len, section, key, buf, sizeof(buf));
    if (rc < 0) {
        return rc;
    }
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    if (*p == '\0') {
        return INI_ERR_VALUE;
    }
    for (; *p != '\0'; p++) {
        d = hex_digit(*p);
        if (d < 0 || (unsigned)d >= base) {
            return INI_ERR_VALUE;
        }
        v = v * base + d;
        if (v > 0xffffffffll) {
            return INI_ERR_RANGE;
        }
    }
    if (neg) {
        v = -v;
    }
    if (v < min || v > max) {
        return INI_ERR_RANGE;
    }
    *out = (int32_t)v;
    return 0;
}

static int word_eq(const char *s, const char *lower)
{
    for (; *lower != '\0'; s++, lower++) {
        char c = *s;

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != *lower) {
            return 0;
        }
    }
    return *s == '\0';
}

int ini_get_bool(const char *text, size_t len, const char *section, const char *key,
                 int *out)
{
    static const char *const yes[] = { "true", "yes", "on", "1" };
    static const char *const no[] = { "false", "no", "off", "0" };
    char buf[INI_MAX_VALUE + 1];
    unsigned i;
    int rc;

    if (out == NULL) {
        return INI_ERR_ARG;
    }
    rc = ini_get(text, len, section, key, buf, sizeof(buf));
    if (rc < 0) {
        return rc;
    }
    for (i = 0; i < 4; i++) {
        if (word_eq(buf, yes[i])) {
            *out = 1;
            return 0;
        }
        if (word_eq(buf, no[i])) {
            *out = 0;
            return 0;
        }
    }
    return INI_ERR_VALUE;
}

const char *ini_strerror(int code)
{
    switch (code) {
    case 0:                 return "ok";
    case INI_STOPPED:       return "stopped by handler";
    case INI_ERR_ARG:       return "bad argument";
    case INI_ERR_SYNTAX:    return "syntax error";
    case INI_ERR_TOO_LONG:  return "line, name or value too long";
    case INI_ERR_CHAR:      return "control character";
    case INI_ERR_NOT_FOUND: return "key not found";
    case INI_ERR_SPACE:     return "buffer too small";
    case INI_ERR_VALUE:     return "bad value";
    case INI_ERR_RANGE:     return "value out of range";
    default:                return "unknown error";
    }
}
