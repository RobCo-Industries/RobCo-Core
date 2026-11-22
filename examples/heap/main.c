#include <stdint.h>

#include <robco/alloc.h>
#include <robco/crc32.h>
#include <robco/klibc.h>
#include <robco/uart.h>

#define SLOTS       32
#define STEPS       400

typedef struct {
    unsigned char *p;
    size_t n;
    unsigned char seed;
} slot_t;

static slot_t slots[SLOTS];
static uint32_t rng = 0x2545F491u;

static uint32_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fill(slot_t *s)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        s->p[i] = (unsigned char)(s->seed + i);
    }
}

static int verify(const slot_t *s)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        if (s->p[i] != (unsigned char)(s->seed + i)) {
            return 0;
        }
    }
    return 1;
}

static void print_heap(void)
{
    k_heap_stats_t st;
    int check = k_heap_check();

    k_heap_stats(&st);
    k_printf("@RC stage=heap ev=heap total=%u used=%u free=%u largest=%u check=%d\n",
             (unsigned)st.total, (unsigned)st.used, (unsigned)st.free,
             (unsigned)st.largest_free, check);
}

/* Mixed malloc, realloc and free with pattern checks. Returns bad patterns. */
static int self_test(void)
{
    int bad = 0;
    int step;
    int i;

    for (step = 0; step < STEPS; step++) {
        uint32_t r = next_rand();
        slot_t *s = &slots[r % SLOTS];
        size_t n = 1 + next_rand() % 2048;
        unsigned char *q;

        if (s->p == NULL) {
            s->p = k_malloc(n);
            if (s->p != NULL) {
                s->n = n;
                s->seed = (unsigned char)step;
                fill(s);
            }
        } else if (r & 0x100) {
            bad += !verify(s);
            q = k_realloc(s->p, n);
            if (q != NULL) {
                s->p = q;
                s->n = n < s->n ? n : s->n;
                bad += !verify(s);
                s->n = n;
                fill(s);
            }
        } else {
            bad += !verify(s);
            k_free(s->p);
            s->p = NULL;
        }
    }
    for (i = 0; i < SLOTS; i++) {
        if (slots[i].p != NULL) {
            bad += !verify(&slots[i]);
            k_free(slots[i].p);
            slots[i].p = NULL;
        }
    }
    return bad;
}

void robco_main(void)
{
    void *base;
    size_t size;
    int bad;

    uart_init(115200);
    k_printf("\n");
    /* Everything between the stacks and the end of the slot. */
    size = robco_heap_slot(&base);
    k_printf("@RC stage=heap ev=region base=0x%08x size=%u\n", (unsigned)(uintptr_t)base,
             (unsigned)size);
    k_heap_init(base, size);
    print_heap();
    bad = self_test();
    if (bad != 0) {
        k_printf("@RC stage=heap ev=pattern bad=%d\n", bad);
    }
    print_heap();
    k_printf("@RC stage=heap ev=crc value=0x%08x\n", (unsigned)crc32("123456789", 9));
    k_printf("@RC stage=heap ev=done\n");
    for (;;) {
    }
}
