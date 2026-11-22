#include <stddef.h>

#include <robco/klibc.h>

/*
 * Byte loops on purpose: the library is built with
 * -mno-unaligned-access and these are not hot paths yet.
 */
void *k_memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

void *k_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
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
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
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
