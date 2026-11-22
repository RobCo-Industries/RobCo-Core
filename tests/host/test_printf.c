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
