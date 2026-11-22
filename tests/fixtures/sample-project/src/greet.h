#ifndef SAMPLE_GREET_H
#define SAMPLE_GREET_H

#include <stddef.h>

/* Writes "hello <name> #<n>" into buf. Pure: also built for the host tests. */
int greet(char *buf, size_t size, const char *name, unsigned n);

#endif
