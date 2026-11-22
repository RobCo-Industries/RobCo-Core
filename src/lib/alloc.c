#include <stddef.h>
#include <stdint.h>

#include <robco/alloc.h>
#include <robco/klibc.h>

/*
 * Every block starts with a header. Blocks tile the heap with no gaps, so
 * size walks forward and prev_size walks back. Free blocks also sit on a
 * doubly linked free list kept in their payload.
 *
 * magic mixes a used or free tag with the header address and size, so a
 * stray pointer, a double free or an overwritten header does not match.
 */
typedef struct {
    size_t size;        /* whole block, header included, multiple of 8 */
    size_t prev_size;   /* size of the block before, 0 for the first */
    uint32_t magic;
    uint32_t guard;     /* ~magic */
} k_blk_t;

typedef struct k_free_node {
    struct k_free_node *next;
    struct k_free_node *prev;
} k_free_node_t;

#define K_ALIGN         8u
#define K_HDR           ((sizeof(k_blk_t) + K_ALIGN - 1) & ~(size_t)(K_ALIGN - 1))
#define K_MIN_PAYLOAD   ((sizeof(k_free_node_t) + K_ALIGN - 1) & ~(size_t)(K_ALIGN - 1))
#define K_MIN_BLOCK     (K_HDR + K_MIN_PAYLOAD)
#define K_TAG_USED      0xA110C8EDu
#define K_TAG_FREE      0xF4EEB10Cu

static unsigned char *heap_start;
static unsigned char *heap_end;
static k_free_node_t *free_list;
static size_t bad_frees;

static uint32_t blk_magic(const k_blk_t *b, size_t size, int used)
{
    return (used ? K_TAG_USED : K_TAG_FREE) ^ (uint32_t)(uintptr_t)b ^ (uint32_t)size;
}

static void blk_set(k_blk_t *b, size_t size, int used)
{
    b->size = size;
    b->magic = blk_magic(b, size, used);
    b->guard = ~b->magic;
}

/* 1 used, 0 free, -1 bad header. */
static int blk_state(const k_blk_t *b)
{
    if (b->guard != ~b->magic) {
        return -1;
    }
    if (b->magic == blk_magic(b, b->size, 1)) {
        return 1;
    }
    if (b->magic == blk_magic(b, b->size, 0)) {
        return 0;
    }
    return -1;
}

static k_free_node_t *blk_node(k_blk_t *b)
{
    return (k_free_node_t *)((unsigned char *)b + K_HDR);
}

static k_blk_t *node_blk(k_free_node_t *n)
{
    return (k_blk_t *)((unsigned char *)n - K_HDR);
}

static k_blk_t *blk_next(k_blk_t *b)
{
    unsigned char *p = (unsigned char *)b + b->size;

    return p < heap_end ? (k_blk_t *)p : NULL;
}

static k_blk_t *blk_prev(k_blk_t *b)
{
    return b->prev_size ? (k_blk_t *)((unsigned char *)b - b->prev_size) : NULL;
}

static void list_push(k_blk_t *b)
{
    k_free_node_t *n = blk_node(b);

    n->prev = NULL;
    n->next = free_list;
    if (free_list != NULL) {
        free_list->prev = n;
    }
    free_list = n;
}

static void list_remove(k_blk_t *b)
{
    k_free_node_t *n = blk_node(b);

    if (n->prev != NULL) {
        n->prev->next = n->next;
    } else {
        free_list = n->next;
    }
    if (n->next != NULL) {
        n->next->prev = n->prev;
    }
}

static void set_next_prev_size(k_blk_t *b)
{
    k_blk_t *next = blk_next(b);

    if (next != NULL) {
        next->prev_size = b->size;
    }
}

/* Marks b free, merges it with free neighbours and puts it on the list. */
static void blk_release(k_blk_t *b)
{
    k_blk_t *next = blk_next(b);
    k_blk_t *prev = blk_prev(b);
    size_t size = b->size;

    if (next != NULL && blk_state(next) == 0) {
        list_remove(next);
        size += next->size;
    }
    if (prev != NULL && blk_state(prev) == 0) {
        list_remove(prev);
        size += prev->size;
        b = prev;
    }
    blk_set(b, size, 0);
    set_next_prev_size(b);
    list_push(b);
}

/* Shrinks used block b to size and frees the tail if it is big enough. */
static void blk_trim(k_blk_t *b, size_t size)
{
    k_blk_t *tail;

    if (b->size - size < K_MIN_BLOCK) {
        return;
    }
    tail = (k_blk_t *)((unsigned char *)b + size);
    tail->size = b->size - size;
    tail->prev_size = size;
    blk_set(b, size, 1);
    blk_set(tail, tail->size, 1);
    set_next_prev_size(tail);
    blk_release(tail);
}

/* Block size for an n-byte request, or 0 if it cannot fit any heap. */
static size_t blk_size_for(size_t n)
{
    if (n > (size_t)-1 - K_HDR - K_ALIGN) {
        return 0;
    }
    n = (n + K_ALIGN - 1) & ~(size_t)(K_ALIGN - 1);
    if (n < K_MIN_PAYLOAD) {
        n = K_MIN_PAYLOAD;
    }
    return K_HDR + n;
}

/* The used block behind p, or NULL (and counted) if p is not one. */
static k_blk_t *ptr_blk(void *p)
{
    unsigned char *c = p;
    k_blk_t *b;

    if (heap_start == NULL || c < heap_start + K_HDR || c >= heap_end || ((uintptr_t)c & (K_ALIGN - 1)) != 0) {
        bad_frees++;
        return NULL;
    }
    b = (k_blk_t *)(c - K_HDR);
    if (blk_state(b) != 1) {
        bad_frees++;
        return NULL;
    }
    return b;
}

void k_heap_init(void *base, size_t size)
{
    uintptr_t start = ((uintptr_t)base + K_ALIGN - 1) & ~(uintptr_t)(K_ALIGN - 1);
    uintptr_t end = ((uintptr_t)base + size) & ~(uintptr_t)(K_ALIGN - 1);
    k_blk_t *b;

    free_list = NULL;
    bad_frees = 0;
    heap_start = heap_end = NULL;
    if (base == NULL || (uintptr_t)base + size < (uintptr_t)base ||
        end < start || end - start < K_MIN_BLOCK) {
        return;
    }
    heap_start = (unsigned char *)start;
    heap_end = (unsigned char *)end;
    b = (k_blk_t *)heap_start;
    b->prev_size = 0;
    blk_set(b, end - start, 0);
    list_push(b);
}

void *k_malloc(size_t n)
{
    size_t need = blk_size_for(n);
    k_free_node_t *node;
    k_blk_t *b;

    if (n == 0 || need == 0) {
        return NULL;
    }
    for (node = free_list; node != NULL; node = node->next) {
        b = node_blk(node);
        if (b->size >= need) {
            list_remove(b);
            blk_set(b, b->size, 1);
            blk_trim(b, need);
            return (unsigned char *)b + K_HDR;
        }
    }
    return NULL;
}

void *k_calloc(size_t n, size_t size)
{
    void *p;

    if (size != 0 && n > (size_t)-1 / size) {
        return NULL;
    }
    p = k_malloc(n * size);
    if (p != NULL) {
        k_memset(p, 0, n * size);
    }
    return p;
}

void k_free(void *p)
{
    k_blk_t *b;

    if (p == NULL) {
        return;
    }
    b = ptr_blk(p);
    if (b != NULL) {
        blk_release(b);
    }
}

void *k_realloc(void *p, size_t n)
{
    k_blk_t *b;
    k_blk_t *next;
    size_t need;
    void *q;

    if (p == NULL) {
        return k_malloc(n);
    }
    if (n == 0) {
        k_free(p);
        return NULL;
    }
    b = ptr_blk(p);
    if (b == NULL) {
        return NULL;
    }
    need = blk_size_for(n);
    if (need == 0) {
        return NULL;
    }
    if (need <= b->size) {
        blk_trim(b, need);
        return p;
    }
    next = blk_next(b);
    if (next != NULL && blk_state(next) == 0 && b->size + next->size >= need) {
        list_remove(next);
        blk_set(b, b->size + next->size, 1);
        set_next_prev_size(b);
        blk_trim(b, need);
        return p;
    }
    q = k_malloc(n);
    if (q == NULL) {
        return NULL;
    }
    k_memcpy(q, p, b->size - K_HDR);
    blk_release(b);
    return q;
}

void k_heap_stats(k_heap_stats_t *out)
{
    unsigned char *c;
    k_blk_t *b;

    k_memset(out, 0, sizeof(*out));
    out->total = (size_t)(heap_end - heap_start);
    out->bad_frees = bad_frees;
    for (c = heap_start; c < heap_end; c += b->size) {
        size_t payload;

        b = (k_blk_t *)c;
        if (b->size < K_MIN_BLOCK) {
            break;      /* corrupt: k_heap_check reports it */
        }
        payload = b->size - K_HDR;
        if (blk_state(b) == 1) {
            out->used += payload;
            out->allocs++;
        } else {
            out->free += payload;
            if (payload > out->largest_free) {
                out->largest_free = payload;
            }
        }
    }
}

int k_heap_check(void)
{
    unsigned char *c;
    k_blk_t *b;
    k_free_node_t *n;
    k_free_node_t *prev = NULL;
    size_t prev_size = 0;
    size_t nfree = 0;
    size_t nlist = 0;
    int prev_free = 0;
    int state;

    for (c = heap_start; c < heap_end; c += b->size) {
        b = (k_blk_t *)c;
        state = blk_state(b);
        if (state < 0) {
            return -1;
        }
        if (b->size < K_MIN_BLOCK || (b->size & (K_ALIGN - 1)) != 0 ||
            b->size > (size_t)(heap_end - c)) {
            return -2;
        }
        if (b->prev_size != prev_size) {
            return -3;
        }
        if (state == 0) {
            if (prev_free) {
                return -4;  /* two free neighbours were not merged */
            }
            nfree++;
        }
        prev_free = state == 0;
        prev_size = b->size;
    }
    if (c != heap_end) {
        return -5;
    }
    for (n = free_list; n != NULL; n = n->next) {
        unsigned char *bc = (unsigned char *)node_blk(n);

        if (bc < heap_start || bc >= heap_end || ++nlist > nfree) {
            return -6;
        }
        if (blk_state(node_blk(n)) != 0 || n->prev != prev) {
            return -7;
        }
        prev = n;
    }
    if (nlist != nfree) {
        return -8;
    }
    return 0;
}
