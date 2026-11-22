/*
 * strchr() for FatFs on the target, in its own file so the linker pulls it
 * only when FatFs needs it and a project can supply its own. The host uses
 * its C library.
 */
#ifndef ROBCO_HOST
#include <stddef.h>

char *strchr(const char *s, int c);

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)(size_t)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}
#endif
