#ifndef ROBCO_ALLOC_H
#define ROBCO_ALLOC_H

#include <stddef.h>

/*
 * First-fit heap over one memory region. Blocks are 8-byte aligned.
 * No locking: callers on a single core mask IRQs around heap calls.
 *
 * k_malloc(0) returns NULL. k_free(NULL) does nothing.
 * k_realloc(NULL, n) is k_malloc(n). k_realloc(p, 0) frees p and returns NULL.
 * k_free and k_realloc ignore a pointer the heap did not hand out, or one
 * already freed, and count it in bad_frees.
 */
void k_heap_init(void *base, size_t size);
void *k_malloc(size_t n);
void *k_calloc(size_t n, size_t size);
void *k_realloc(void *p, size_t n);
void k_free(void *p);

/*
 * Bytes are payload bytes: block headers are not counted.
 * total: the whole region managed by the heap, headers included.
 */
typedef struct {
    size_t total, used, free, largest_free, allocs;
    size_t bad_frees;
} k_heap_stats_t;

void k_heap_stats(k_heap_stats_t *out);

/* Walks the heap. Returns 0 if consistent, a negative code otherwise. */
int k_heap_check(void);

#endif
