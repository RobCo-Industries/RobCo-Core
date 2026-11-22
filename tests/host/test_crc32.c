#include <robco/crc32.h>

#include "rc_test.h"

static uint32_t crc32_slow(const unsigned char *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    int k;

    while (len--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

TEST(check_value)
{
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32_update(0, "123456789", 9), 0xCBF43926u);
}

TEST(empty)
{
    CHECK_EQ(crc32("", 0), 0);
    CHECK_EQ(crc32(NULL, 0), 0);
    CHECK_EQ(crc32_update(0x12345678u, "", 0), 0x12345678u);
}

TEST(streaming)
{
    const char *s = "The quick brown fox jumps over the lazy dog";
    size_t len = strlen(s);
    size_t chunk, off;

    CHECK_EQ(crc32(s, len), 0x414FA339u);
    for (chunk = 1; chunk <= len; chunk++) {
        uint32_t c = 0;

        for (off = 0; off < len; off += chunk) {
            size_t n = len - off < chunk ? len - off : chunk;
            c = crc32_update(c, s + off, n);
        }
        CHECK_EQ(c, 0x414FA339u);
    }
}

TEST(random_1mib)
{
    size_t len = 1u << 20;
    unsigned char *buf = malloc(len);
    uint32_t x = 0x2545F491u;
    uint32_t c = 0;
    size_t i;

    CHECK(buf != NULL);
    if (buf == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (unsigned char)x;
    }
    CHECK_EQ(crc32(buf, len), crc32_slow(buf, len));
    for (i = 0; i < len; i += 4093) {
        c = crc32_update(c, buf + i, len - i < 4093 ? len - i : 4093);
    }
    CHECK_EQ(c, crc32_slow(buf, len));
    free(buf);
}
