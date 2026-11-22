#include <stddef.h>

/*
 * GCC may emit calls to memset/memcpy even with -ffreestanding,
 * so these two keep their libc names.
 */
void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}
