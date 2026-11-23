#include <stddef.h>

#include <robco/klibc.h>

#include <stdint.h>

/*
 * Word loops where both pointers can be word aligned, byte loops
 * otherwise. The library is built with -mno-unaligned-access, so a word
 * access is only made at an aligned address. word_t may alias any object.
 */
typedef uint32_t __attribute__((may_alias)) word_t;

#define WORD_MASK ((uintptr_t)(sizeof(word_t) - 1u))

void *k_memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    word_t w = (unsigned char)value;

    while (n != 0 && ((uintptr_t)d & WORD_MASK) != 0) {
        *d++ = (unsigned char)value;
        n--;
    }
    w |= w << 8;
    w |= w << 16;
    while (n >= sizeof(word_t)) {
        *(word_t *)(void *)d = w;
        d += sizeof(word_t);
        n -= sizeof(word_t);
    }
    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

/* Forward copy. Words when both pointers share their misalignment. */
static void copy_up(unsigned char *d, const unsigned char *s, size_t n)
{
    if ((((uintptr_t)d ^ (uintptr_t)s) & WORD_MASK) == 0) {
        while (n != 0 && ((uintptr_t)d & WORD_MASK) != 0) {
            *d++ = *s++;
            n--;
        }
        while (n >= sizeof(word_t)) {
            *(word_t *)(void *)d = *(const word_t *)(const void *)s;
            d += sizeof(word_t);
            s += sizeof(word_t);
            n -= sizeof(word_t);
        }
    }
    while (n--) {
        *d++ = *s++;
    }
}

/* Backward copy from the ends `d` and `s`, for an overlap with d above s. */
static void copy_down(unsigned char *d, const unsigned char *s, size_t n)
{
    if ((((uintptr_t)d ^ (uintptr_t)s) & WORD_MASK) == 0) {
        while (n != 0 && ((uintptr_t)d & WORD_MASK) != 0) {
            *--d = *--s;
            n--;
        }
        while (n >= sizeof(word_t)) {
            d -= sizeof(word_t);
            s -= sizeof(word_t);
            *(word_t *)(void *)d = *(const word_t *)(const void *)s;
            n -= sizeof(word_t);
        }
    }
    while (n--) {
        *--d = *--s;
    }
}

void *k_memcpy(void *dst, const void *src, size_t n)
{
    copy_up(dst, src, n);
    return dst;
}

void *k_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        copy_up(d, s, n);
    } else {
        copy_down(d + n, s + n, n);
    }
    return dst;
}

int k_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a;
    const unsigned char *y = b;

    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x < *y ? -1 : 1;
        }
    }
    return 0;
}

size_t k_strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

int k_strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) {
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        }
        if (*a == '\0') {
            break;
        }
    }
    return 0;
}

int k_strcmp(const char *a, const char *b)
{
    return k_strncmp(a, b, (size_t)-1);
}

char *k_strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

/*
 * GCC may emit calls to these even with -ffreestanding. The host test
 * build sets ROBCO_HOST and uses the host libc ones instead.
 */
#ifndef ROBCO_HOST
void *memset(void *dst, int value, size_t n)
{
    return k_memset(dst, value, n);
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return k_memcpy(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    return k_memmove(dst, src, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    return k_memcmp(a, b, n);
}
#endif
