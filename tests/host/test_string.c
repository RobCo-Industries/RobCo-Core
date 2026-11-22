#include <robco/klibc.h>

#include "rc_test.h"

static void fill(unsigned char *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(seed + i * 7);
    }
}

TEST(memset_sizes)
{
    static const size_t sizes[] = { 0, 1, 3, 7, 31, 33 };
    unsigned char buf[40];
    size_t i, j;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memset(buf, 0xAA, sizeof(buf));
        CHECK(k_memset(buf + 1, 0x1234, sizes[i]) == buf + 1);
        CHECK_EQ(buf[0], 0xAA);
        for (j = 0; j < sizes[i]; j++) {
            CHECK_EQ(buf[1 + j], 0x34);
        }
        CHECK_EQ(buf[1 + sizes[i]], 0xAA);
    }
}

TEST(memcpy_sizes)
{
    static const size_t sizes[] = { 0, 1, 3, 7, 31, 33 };
    unsigned char src[40], dst[40], ref[40];
    size_t i;

    fill(src, sizeof(src), 5);
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memset(dst, 0x55, sizeof(dst));
        memset(ref, 0x55, sizeof(ref));
        memcpy(ref + 3, src + 1, sizes[i]);
        CHECK(k_memcpy(dst + 3, src + 1, sizes[i]) == dst + 3);
        CHECK_MEM(dst, ref, sizeof(dst));
    }
}

TEST(memmove_forward_overlap)
{
    unsigned char buf[32], ref[32];

    fill(buf, sizeof(buf), 1);
    memcpy(ref, buf, sizeof(buf));
    memmove(ref, ref + 5, 17);
    CHECK(k_memmove(buf, buf + 5, 17) == buf);
    CHECK_MEM(buf, ref, sizeof(buf));
}

TEST(memmove_backward_overlap)
{
    unsigned char buf[32], ref[32];

    fill(buf, sizeof(buf), 2);
    memcpy(ref, buf, sizeof(buf));
    memmove(ref + 5, ref, 17);
    CHECK(k_memmove(buf + 5, buf, 17) == buf + 5);
    CHECK_MEM(buf, ref, sizeof(buf));
}

TEST(memmove_edges)
{
    unsigned char buf[8], ref[8];

    fill(buf, sizeof(buf), 3);
    memcpy(ref, buf, sizeof(buf));
    CHECK(k_memmove(buf, buf, 8) == buf);
    CHECK(k_memmove(buf + 1, buf, 0) == buf + 1);
    CHECK_MEM(buf, ref, sizeof(buf));
    k_memmove(buf + 1, buf, 1);
    CHECK_EQ(buf[1], ref[0]);
    k_memmove(buf + 4, buf + 5, 3);
    CHECK_MEM(buf + 4, ref + 5, 3);
}

TEST(memcmp_order)
{
    unsigned char a[5] = { 1, 2, 3, 4, 5 };
    unsigned char b[5] = { 1, 2, 3, 4, 5 };

    CHECK_EQ(k_memcmp(a, b, 0), 0);
    CHECK_EQ(k_memcmp(a, b, 5), 0);
    b[4] = 0x80;
    CHECK(k_memcmp(a, b, 5) < 0);
    CHECK(k_memcmp(b, a, 5) > 0);
    CHECK_EQ(k_memcmp(a, b, 4), 0);
    a[0] = 0xFF;
    CHECK(k_memcmp(a, b, 1) > 0);
}

TEST(strlen_basic)
{
    CHECK_EQ(k_strlen(""), 0);
    CHECK_EQ(k_strlen("a"), 1);
    CHECK_EQ(k_strlen("hello"), 5);
}

TEST(strcmp_basic)
{
    CHECK_EQ(k_strcmp("", ""), 0);
    CHECK_EQ(k_strcmp("abc", "abc"), 0);
    CHECK(k_strcmp("abc", "abd") < 0);
    CHECK(k_strcmp("abd", "abc") > 0);
    CHECK(k_strcmp("ab", "abc") < 0);
    CHECK(k_strcmp("abc", "ab") > 0);
    CHECK(k_strcmp("a", "\xff") < 0);
}

TEST(strncmp_limits)
{
    CHECK_EQ(k_strncmp("abc", "xyz", 0), 0);
    CHECK_EQ(k_strncmp("abc", "abd", 2), 0);
    CHECK(k_strncmp("abc", "abd", 3) < 0);
    CHECK_EQ(k_strncmp("abc", "abc", 10), 0);
    CHECK(k_strncmp("ab", "abc", 3) < 0);
    CHECK_EQ(k_strncmp("ab", "abc", 2), 0);
    CHECK(k_strncmp("\x80", "\x01", 1) > 0);
}

TEST(strncpy_padding)
{
    char buf[8];

    memset(buf, 'x', sizeof(buf));
    CHECK(k_strncpy(buf, "ab", 6) == buf);
    CHECK_MEM(buf, "ab\0\0\0\0xx", 8);
}

TEST(strncpy_truncation)
{
    char buf[8];

    memset(buf, 'x', sizeof(buf));
    k_strncpy(buf, "abcdefgh", 3);
    CHECK_MEM(buf, "abcxxxxx", 8);
    memset(buf, 'x', sizeof(buf));
    k_strncpy(buf, "abc", 3);
    CHECK_MEM(buf, "abcxxxxx", 8);
    memset(buf, 'x', sizeof(buf));
    k_strncpy(buf, "abc", 0);
    CHECK_MEM(buf, "xxxxxxxx", 8);
    k_strncpy(buf, "", 1);
    CHECK_MEM(buf, "\0xxxxxxx", 8);
}
