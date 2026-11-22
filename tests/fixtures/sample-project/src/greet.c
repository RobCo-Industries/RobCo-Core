#include <robco/klibc.h>

#include "greet.h"

int greet(char *buf, size_t size, const char *name, unsigned n)
{
    return k_snprintf(buf, size, "hello %s #%u", name, n);
}
