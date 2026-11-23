/*
 * k_memcpy, k_memmove and k_memset against byte-wise references, for
 * every source and destination offset 0 to 7 and length 0 to 67, with
 * guard bytes around the destination.
 */
#include <stdint.h>

#include <robco/klibc.h>

#include <stdio.h>
#include <string.h>

#include "rc_test.h"

#define MAXLEN  67
#define GUARD   16
#define SIZE    (GUARD + 8 + MAXLEN + GUARD + 8)

static uint32_t a_words[SIZE / 4 + 1];
static uint32_t b_words[SIZE / 4 + 1];
static unsigned char *const a = (unsigned char *)a_words;
static unsigned char *const b = (unsigned char *)b_words;

static void pattern(unsigned char *p, unsigned seed)
{
    unsigned i;

    for (i = 0; i < SIZE; i++) {
        p[i] = (unsigned char)(i * 37u + seed);
    }
}

TEST(memcpy_every_offset_and_length)
{
    static unsigned char src[SIZE + 4];
    unsigned so, d, n, i;

    for (i = 0; i < sizeof(src); i++) {
        src[i] = (unsigned char)(i * 11u + 3u);
    }
    for (so = 0; so < 8; so++) {
        for (d = 0; d < 8; d++) {
            for (n = 0; n <= MAXLEN; n++) {
                pattern(a, 1);
                pattern(b, 1);
                for (i = 0; i < n; i++) {
                    b[GUARD + d + i] = src[so + i];
                }
                CHECK(k_memcpy(a + GUARD + d, src + so, n) == a + GUARD + d);
                if (memcmp(a, b, SIZE) != 0) {
                    RC_FAIL("memcpy src %u dst %u len %u", so, d, n);
                    return;
                }
            }
        }
    }
}

/* Both directions of overlap: source and destination in one buffer. */
TEST(memmove_overlap_every_offset_and_length)
{
    static unsigned char tmp[SIZE];
    unsigned so, d, n, i;

    for (so = 0; so < 8; so++) {
        for (d = 0; d < 8; d++) {
            for (n = 0; n <= MAXLEN; n++) {
                pattern(a, 5);
                pattern(b, 5);
                /* Forward overlap (dst below src) when d < so, backward when d > so. */
                for (i = 0; i < n; i++) {
                    tmp[i] = b[GUARD + so + i];
                }
                for (i = 0; i < n; i++) {
                    b[GUARD + d + i] = tmp[i];
                }
                CHECK(k_memmove(a + GUARD + d, a + GUARD + so, n) == a + GUARD + d);
                if (memcmp(a, b, SIZE) != 0) {
                    RC_FAIL("memmove src %u dst %u len %u", so, d, n);
                    return;
                }
                /* Overlap by whole words too: shift by 4 or 8 bytes. */
                pattern(a, 9);
                pattern(b, 9);
                for (i = 0; i < n; i++) {
                    tmp[i] = b[GUARD + d + i];
                }
                for (i = 0; i < n; i++) {
                    b[GUARD + d + 8 - so + i] = tmp[i];
                }
                k_memmove(a + GUARD + d + 8 - so, a + GUARD + d, n);
                if (memcmp(a, b, SIZE) != 0) {
                    RC_FAIL("memmove up src %u dst %u len %u", d, d + 8 - so, n);
                    return;
                }
            }
        }
    }
}

TEST(memset_every_offset_and_length)
{
    unsigned d, n, i;

    for (d = 0; d < 8; d++) {
        for (n = 0; n <= MAXLEN; n++) {
            pattern(a, 7);
            pattern(b, 7);
            for (i = 0; i < n; i++) {
                b[GUARD + d + i] = 0xc3;
            }
            CHECK(k_memset(a + GUARD + d, 0x1c3, n) == a + GUARD + d);
            if (memcmp(a, b, SIZE) != 0) {
                RC_FAIL("memset dst %u len %u", d, n);
                return;
            }
        }
    }
}
