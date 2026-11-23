#include <limits.h>
#include <stdint.h>

#include <robco/klibc.h>

#include "rc_test.h"

static char out[128];

/* Formats into out and checks the text and the return value. */
#define FMT(expect, ...)                                                  \
    do {                                                                  \
        int rc_n = k_snprintf(out, sizeof(out), __VA_ARGS__);             \
        CHECK_STR(out, expect);                                           \
        CHECK_EQ(rc_n, strlen(expect));                                   \
    } while (0)

/* Formats through a non-literal string to test what the compiler rejects. */
static int fmt_raw(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = k_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

TEST(plain_text)
{
    FMT("", "%s", "");
    FMT("hello", "hello");
    FMT("100%", "100%%");
}

TEST(strings)
{
    FMT("abc", "%s", "abc");
    FMT("  abc", "%5s", "abc");
    FMT("abc", "%2s", "abc");
    fmt_raw(out, sizeof(out), "%05s", "abc");
    CHECK_STR(out, "  abc");
    FMT("(null)", "%s", (const char *)rc_launder(NULL));
}

TEST(chars)
{
    FMT("x", "%c", 'x');
    FMT("a-b", "%c-%c", 'a', 'b');
}

TEST(signed_decimal)
{
    FMT("0", "%d", 0);
    FMT("42", "%d", 42);
    FMT("-42", "%d", -42);
    FMT("2147483647", "%d", INT_MAX);
    FMT("-2147483648", "%d", INT_MIN);
    FMT("   -7", "%5d", -7);
    FMT("-0007", "%05d", -7);
    FMT("00042", "%05d", 42);
    FMT("-42", "%2d", -42);
}

TEST(unsigned_decimal)
{
    FMT("0", "%u", 0u);
    FMT("4294967295", "%u", 0xFFFFFFFFu);
    FMT("   12", "%5u", 12u);
}

TEST(hex)
{
    FMT("0", "%x", 0u);
    FMT("ffffffff", "%x", 0xFFFFFFFFu);
    FMT("FFFFFFFF", "%X", 0xFFFFFFFFu);
    FMT("00000000", "%08x", 0u);
    FMT("0000beef", "%08x", 0xbeefu);
    FMT("    BEEF", "%8X", 0xbeefu);
}

TEST(pointer)
{
    char expect[64];
    void *p = (void *)(uintptr_t)0x12345678u;

    snprintf(expect, sizeof(expect), "0x%0*llx", (int)(2 * sizeof(void *)),
             (unsigned long long)(uintptr_t)p);
    FMT(expect, "%p", p);
    snprintf(expect, sizeof(expect), "0x%0*llx", (int)(2 * sizeof(void *)),
             (unsigned long long)(uintptr_t)&out);
    FMT(expect, "%p", (void *)&out);
    snprintf(expect, sizeof(expect), "0x%0*llx", (int)(2 * sizeof(void *)), 0ull);
    FMT(expect, "%p", (void *)NULL);
}

TEST(unknown_format)
{
    int n;

    n = fmt_raw(out, sizeof(out), "a%qb");
    CHECK_STR(out, "a%qb");
    CHECK_EQ(n, 4);
    n = fmt_raw(out, sizeof(out), "end%");
    CHECK_STR(out, "end");
    CHECK_EQ(n, 3);
}

TEST(truncation)
{
    char buf[6];
    int n;

    memset(buf, 'x', sizeof(buf));
    n = k_snprintf(buf, sizeof(buf), "%s-%d", "abcdef", 123);
    CHECK_EQ(n, 10);
    CHECK_STR(buf, "abcde");
    n = k_snprintf(buf, sizeof(buf), "%d", 12345);
    CHECK_EQ(n, 5);
    CHECK_STR(buf, "12345");
    n = k_snprintf(buf, sizeof(buf), "%d", 123456);
    CHECK_EQ(n, 6);
    CHECK_STR(buf, "12345");
}

TEST(size_zero)
{
    char buf[2] = { 'x', 'y' };
    int n;

    n = k_snprintf(buf, 0, "hello");
    CHECK_EQ(n, 5);
    CHECK_EQ(buf[0], 'x');
    n = k_snprintf(NULL, 0, "%d", -12);
    CHECK_EQ(n, 3);
}

TEST(size_one)
{
    char buf[2] = { 'x', 'y' };
    int n;

    n = k_snprintf(buf, 1, "hello");
    CHECK_EQ(n, 5);
    CHECK_EQ(buf[0], '\0');
    CHECK_EQ(buf[1], 'y');
}

TEST(left_align)
{
    FMT("abc  |", "%-5s|", "abc");
    FMT("abc|", "%-2s|", "abc");
    FMT("x  |", "%-3c|", 'x');
    FMT("  x|", "%3c|", 'x');
    FMT("-7   |", "%-5d|", -7);
    FMT("42   |", "%-5u|", 42u);
    FMT("beef    |", "%-8x|", 0xbeefu);
    /* '-' beats '0': pads on the right with spaces. */
    fmt_raw(out, sizeof(out), "%-05d|", -7);
    CHECK_STR(out, "-7   |");
    fmt_raw(out, sizeof(out), "%0-5d|", 42);
    CHECK_STR(out, "42   |");
}

TEST(string_precision)
{
    FMT("abcde", "%.5s", "abcdefgh");
    FMT("", "%.0s", "abc");
    FMT("abc", "%.10s", "abc");
    FMT("   ab", "%5.2s", "abc");
    FMT("ab   |", "%-5.2s|", "abc");
    FMT("(nu", "%.3s", (const char *)rc_launder(NULL));
}

TEST(string_precision_unterminated)
{
    const char raw[3] = { 'a', 'b', 'c' };

    FMT("abc", "%.3s", raw);
    FMT("ab", "%.*s", 2, raw);
}

TEST(star_width_and_precision)
{
    FMT("   42", "%*d", 5, 42);
    FMT("42   |", "%-*d|", 5, 42);
    FMT("ab   |", "%-*s|", 5, "ab");
    FMT("abc", "%.*s", 3, "abcdef");
    FMT("  abc", "%*.*s", 5, 3, "abcdef");
    /* A negative '*' width means left-align. */
    FMT("42   |", "%*d|", -5, 42);
    FMT("ab  |", "%*s|", -4, "ab");
    /* A negative '*' precision means no precision. */
    FMT("abcdef", "%.*s", -1, "abcdef");
    FMT("00042", "%0*d", 5, 42);
}

TEST(integer_precision)
{
    FMT("007", "%.3d", 7);
    FMT("-007", "%.3d", -7);
    FMT("  007", "%5.3d", 7);
    FMT("007  |", "%-5.3u|", 7u);
    FMT("00beef", "%.6x", 0xbeefu);
    FMT("12345", "%.2d", 12345);
    FMT("", "%.0d", 0);
    FMT("   ", "%3.0x", 0u);
    FMT("0", "%.1u", 0u);
    /* A precision turns the '0' flag off. */
    fmt_raw(out, sizeof(out), "%05.3d", 7);
    CHECK_STR(out, "  007");
}

TEST(unknown_format_keeps_spec)
{
    int n;

    n = fmt_raw(out, sizeof(out), "a%-5qb");
    CHECK_STR(out, "a%-5qb");
    CHECK_EQ(n, 6);
    n = fmt_raw(out, sizeof(out), "a%.3qb");
    CHECK_STR(out, "a%.3qb");
    n = fmt_raw(out, sizeof(out), "x%5");
    CHECK_STR(out, "x");
}

/* Compares k_snprintf with the host snprintf for the supported subset. */
TEST(matches_host_snprintf)
{
    static const char *const int_fmts[] = {
        "%d", "%5d", "%-5d", "%05d", "%.3d", "%5.3d", "%-5.3d", "%.0d",
        "%u", "%-8u", "%08u", "%.4u", "%x", "%-6x", "%06x", "%.5x",
        "%X", "%-6X", "%.0X", "%1d", "%-1d",
    };
    static const int ints[] = { 0, 1, -1, 7, -42, 12345, INT_MAX, INT_MIN };
    static const char *const str_fmts[] = {
        "%s", "%8s", "%-8s", "%.0s", "%.3s", "%.20s", "%8.3s", "%-8.3s", "%2s",
    };
    static const char *const strs[] = { "", "a", "abc", "abcdefghij" };
    char expect[128];
    size_t i;
    size_t j;
    int n;

    for (i = 0; i < sizeof(int_fmts) / sizeof(int_fmts[0]); i++) {
        for (j = 0; j < sizeof(ints) / sizeof(ints[0]); j++) {
            int en = snprintf(expect, sizeof(expect), int_fmts[i], ints[j]);
            n = fmt_raw(out, sizeof(out), int_fmts[i], ints[j]);
            CHECK_STR(out, expect);
            CHECK_EQ(n, en);
        }
    }
    for (i = 0; i < sizeof(str_fmts) / sizeof(str_fmts[0]); i++) {
        for (j = 0; j < sizeof(strs) / sizeof(strs[0]); j++) {
            int en = snprintf(expect, sizeof(expect), str_fmts[i], strs[j]);
            n = fmt_raw(out, sizeof(out), str_fmts[i], strs[j]);
            CHECK_STR(out, expect);
            CHECK_EQ(n, en);
        }
    }
    for (j = 0; j < sizeof(ints) / sizeof(ints[0]); j++) {
        int en = snprintf(expect, sizeof(expect), "%*d|%-*d|%.*d|%*s|%.*s",
                          6, ints[j], -6, ints[j], 4, ints[j], -5, "ab", 1, "xyz");
        n = fmt_raw(out, sizeof(out), "%*d|%-*d|%.*d|%*s|%.*s",
                    6, ints[j], -6, ints[j], 4, ints[j], -5, "ab", 1, "xyz");
        CHECK_STR(out, expect);
        CHECK_EQ(n, en);
    }
}
