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

/*
 * Heap regions that respect docs/spec/MEMORY_MAP.md. Hardware side,
 * src/board/heap_region.c. Each stores the start in *base and returns the
 * size in bytes, or returns 0 when there is no such region.
 */

/* Start of the region above the image slots, the OS heap in the memory map. */
#define ROBCO_HEAP_HIGH_BASE    0x01000000u

/*
 * From __heap_start up to the end of the image's slot (ROBCO_SLOT_END).
 * Returns 0 if the image does not set ROBCO_SLOT_END.
 */
size_t robco_heap_slot(void **base);

/*
 * From ROBCO_HEAP_HIGH_BASE up to the top of ARM RAM from the mailbox.
 * Only for the image that owns that range in its chain, or one that runs
 * alone. Returns 0 if the mailbox gave no memory size.
 */
size_t robco_heap_high(void **base);

#endif
