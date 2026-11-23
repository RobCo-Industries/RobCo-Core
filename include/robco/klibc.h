#ifndef ROBCO_KLIBC_H
#define ROBCO_KLIBC_H

#include <stdarg.h>
#include <stddef.h>

void *k_memcpy(void *dst, const void *src, size_t n);
void *k_memset(void *dst, int value, size_t n);
void *k_memmove(void *dst, const void *src, size_t n);
int k_memcmp(const void *a, const void *b, size_t n);
size_t k_strlen(const char *s);
int k_strcmp(const char *a, const char *b);
int k_strncmp(const char *a, const char *b, size_t n);
char *k_strncpy(char *dst, const char *src, size_t n);

/* GCC emits calls to these even with -ffreestanding. */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/*
 * Formats %s %c %d %u %x %X %p %% with the '-' and '0' flags, a field width
 * and a precision, each as in C. '*' reads the width or precision from an int
 * argument. Integer precision is the minimum digit count. %p ignores flags,
 * width and precision. An unknown conversion prints as written.
 * k_printf writes to the console hook and drops output while none is set.
 */
void k_console_set(void (*putc)(char));
/* Sets the console only if none is set. uart_init() uses it. */
void k_console_set_default(void (*putc)(char));
void k_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int k_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int k_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
