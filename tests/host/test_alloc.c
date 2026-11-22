#include <stdint.h>

#include <robco/alloc.h>

#include "rc_test.h"

#define ARENA_SIZE (256u * 1024u)

static uint64_t arena_words[ARENA_SIZE / 8];
static unsigned char *const arena = (unsigned char *)arena_words;

static k_heap_stats_t stats(void)
{
    k_heap_stats_t s;

    k_heap_stats(&s);
    return s;
}

static size_t setup(void)
{
    k_heap_init(arena, ARENA_SIZE);
    CHECK_EQ(k_heap_check(), 0);
    return stats().largest_free;
}

TEST(init_stats)
{
    k_heap_stats_t s;

    setup();
    s = stats();
    CHECK_EQ(s.total, ARENA_SIZE);
    CHECK_EQ(s.used, 0);
    CHECK_EQ(s.allocs, 0);
    CHECK_EQ(s.free, s.largest_free);
    CHECK(s.largest_free > ARENA_SIZE - 64);
    CHECK_EQ(s.bad_frees, 0);
}

TEST(unaligned_init)
{
    k_heap_init(arena + 3, 1000);
    CHECK_EQ(k_heap_check(), 0);
    CHECK_EQ((uintptr_t)k_malloc(10) % 8, 0);
    CHECK_EQ(k_heap_check(), 0);
    k_heap_init(arena, 8);
    CHECK(k_malloc(1) == NULL);
    CHECK_EQ(k_heap_check(), 0);
}

TEST(basic_alloc_free)
{
    size_t initial = setup();
    char *a = k_malloc(10);
    char *b = k_malloc(100);

    CHECK(a != NULL && b != NULL && a != b);
    memset(a, 'a', 10);
    memset(b, 'b', 100);
    CHECK_EQ(stats().allocs, 2);
    CHECK(stats().used >= 110);
    CHECK_EQ(k_heap_check(), 0);
    k_free(a);
    k_free(b);
    CHECK_EQ(k_heap_check(), 0);
    CHECK_EQ(stats().used, 0);
    CHECK_EQ(stats().largest_free, initial);
    k_free(NULL);
    CHECK_EQ(stats().bad_frees, 0);
}

TEST(alignment)
{
    size_t n;

    setup();
    for (n = 1; n < 100; n++) {
        void *p = k_malloc(n);

        CHECK(p != NULL);
        CHECK_EQ((uintptr_t)p % 8, 0);
    }
    CHECK_EQ(k_heap_check(), 0);
}

TEST(malloc_zero_returns_null)
{
    setup();
    CHECK(k_malloc(0) == NULL);
    CHECK_EQ(stats().allocs, 0);
}

TEST(exhaustion)
{
    size_t initial = setup();
    void *ptrs[400];
    int n = 0;

    CHECK(k_malloc(initial + 1) == NULL);
    CHECK(k_malloc((size_t)-1) == NULL);
    CHECK(k_malloc((size_t)-8) == NULL);
    while (n < 400 && (ptrs[n] = k_malloc(1000)) != NULL) {
        n++;
    }
    CHECK(n > 200 && n < 400);
    CHECK(k_malloc(1000) == NULL);
    CHECK_EQ(k_heap_check(), 0);
    while (n--) {
        k_free(ptrs[n]);
    }
    CHECK_EQ(stats().largest_free, initial);
    ptrs[0] = k_malloc(initial);
    CHECK(ptrs[0] != NULL);
    k_free(ptrs[0]);
    CHECK_EQ(k_heap_check(), 0);
}

TEST(realloc_cases)
{
    size_t initial = setup();
    unsigned char *p, *q, *blocker;
    size_t i;

    p = k_realloc(NULL, 16);
    CHECK(p != NULL);
    for (i = 0; i < 16; i++) {
        p[i] = (unsigned char)i;
    }
    /* Grow in place: the rest of the heap is free after p. */
    q = k_realloc(p, 200);
    CHECK(q == p);
    for (i = 0; i < 16; i++) {
        CHECK_EQ(q[i], i);
    }
    /* Shrink in place. */
    q = k_realloc(p, 8);
    CHECK(q == p);
    CHECK_EQ(q[7], 7);
    CHECK_EQ(k_heap_check(), 0);
    /* Grow with a used neighbour: moves and copies. */
    blocker = k_malloc(32);
    q = k_realloc(p, 500);
    CHECK(q != NULL && q != p);
    for (i = 0; i < 8; i++) {
        CHECK_EQ(q[i], i);
    }
    CHECK_EQ(k_heap_check(), 0);
    /* Too big: NULL and the old block is kept. */
    CHECK(k_realloc(q, initial * 2) == NULL);
    CHECK_EQ(q[3], 3);
    /* Zero frees. */
    CHECK(k_realloc(q, 0) == NULL);
    k_free(blocker);
    CHECK_EQ(stats().used, 0);
    CHECK_EQ(stats().largest_free, initial);
    CHECK_EQ(stats().bad_frees, 0);
    CHECK_EQ(k_heap_check(), 0);
}

TEST(calloc_zero_and_overflow)
{
    unsigned char *p;
    size_t i;

    setup();
    p = k_malloc(256);
    memset(p, 0xEE, 256);
    k_free(p);
    p = k_calloc(16, 16);
    CHECK(p != NULL);
    for (i = 0; i < 256; i++) {
        CHECK_EQ(p[i], 0);
    }
    CHECK(k_calloc((size_t)-1 / 2 + 2, 2) == NULL);
    CHECK(k_calloc(2, (size_t)-1 / 2 + 2) == NULL);
    CHECK(k_calloc(0, 16) == NULL);
    CHECK_EQ(k_heap_check(), 0);
}

TEST(double_and_bad_free)
{
    size_t initial = setup();
    char *a = k_malloc(40);
    char *b = k_malloc(40);
    int local = 0;

    k_free(a);
    k_free(rc_launder(a));
    CHECK_EQ(stats().bad_frees, 1);
    CHECK_EQ(k_heap_check(), 0);
    k_free(rc_launder(b + 1));
    k_free(rc_launder(b + 8));
    k_free(rc_launder(&local));
    k_free(rc_launder((void *)((uintptr_t)arena + ARENA_SIZE + 64)));
    CHECK_EQ(stats().bad_frees, 5);
    CHECK_EQ(stats().allocs, 1);
    CHECK_EQ(k_heap_check(), 0);
    CHECK(k_realloc(rc_launder(a), 10) == NULL);
    CHECK_EQ(stats().bad_frees, 6);
    k_free(b);
    CHECK_EQ(stats().largest_free, initial);
    CHECK_EQ(k_heap_check(), 0);
}

TEST(header_corruption_detected)
{
    char *a;
    char *b;

    setup();
    a = k_malloc(24);
    b = k_malloc(24);
    CHECK(b != NULL);
    memset(rc_launder(a), 0x41, 64);   /* overruns into b's header */
    CHECK(k_heap_check() != 0);
    k_free(b);
    CHECK_EQ(stats().bad_frees, 1);
}

TEST(full_coalescing)
{
    size_t initial = setup();
    void *p[64];
    int i;

    for (i = 0; i < 64; i++) {
        p[i] = k_malloc((size_t)(i * 13 + 1));
    }
    /* Free odd, then even in reverse, so merges go both ways. */
    for (i = 1; i < 64; i += 2) {
        k_free(p[i]);
    }
    CHECK_EQ(k_heap_check(), 0);
    for (i = 62; i >= 0; i -= 2) {
        k_free(p[i]);
    }
    CHECK_EQ(k_heap_check(), 0);
    CHECK_EQ(stats().used, 0);
    CHECK_EQ(stats().largest_free, initial);
    CHECK_EQ(stats().free, initial);
}

/* xorshift32 with a fixed seed. */
static uint32_t rng = 0x9E3779B9u;

static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct {
    unsigned char *p;
    size_t n;
    unsigned char seed;
} slot_t;

static int pattern_ok(const slot_t *s)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        if (s->p[i] != (unsigned char)(s->seed + i)) {
            return 0;
        }
    }
    return 1;
}

static void pattern_fill(slot_t *s)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        s->p[i] = (unsigned char)(s->seed + i);
    }
}

TEST(stress)
{
    size_t initial = setup();
    static slot_t slots[128];
    int step, i, bad = 0;

    memset(slots, 0, sizeof(slots));
    for (step = 0; step < 20000; step++) {
        uint32_t r = next_rand();
        slot_t *s = &slots[r % 128];
        size_t n = 1 + (next_rand() % ((r & 0x100) ? 4096 : 64));

        if (s->p == NULL) {
            s->p = k_malloc(n);
            if (s->p != NULL) {
                s->n = n;
                s->seed = (unsigned char)step;
                pattern_fill(s);
            }
        } else if (r & 0x200) {
            unsigned char *q;
            size_t keep;

            bad += !pattern_ok(s);
            q = k_realloc(s->p, n);
            if (q != NULL) {
                keep = n < s->n ? n : s->n;
                s->p = q;
                s->n = keep;
                bad += !pattern_ok(s);
                s->n = n;
                pattern_fill(s);
            } else {
                bad += !pattern_ok(s);
            }
        } else {
            bad += !pattern_ok(s);
            k_free(s->p);
            s->p = NULL;
        }
        if (step % 500 == 0) {
            CHECK_EQ(k_heap_check(), 0);
        }
    }
    CHECK_EQ(bad, 0);
    for (i = 0; i < 128; i++) {
        if (slots[i].p != NULL) {
            CHECK(pattern_ok(&slots[i]));
            k_free(slots[i].p);
        }
    }
    CHECK_EQ(k_heap_check(), 0);
    CHECK_EQ(stats().used, 0);
    CHECK_EQ(stats().largest_free, initial);
    CHECK_EQ(stats().bad_frees, 0);
}
