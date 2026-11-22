/*
 * The target toolchain has no C library headers. ff.c includes <string.h>,
 * so the target build puts this directory on the FatFs include path. The
 * functions come from src/lib/k_string.c and src/fs/fs_strchr.c. Host
 * builds use the host <string.h>.
 */
#ifndef ROBCO_FS_LIBC_STRING_H
#define ROBCO_FS_LIBC_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
char *strchr(const char *s, int c);
size_t strlen(const char *s);

#endif
