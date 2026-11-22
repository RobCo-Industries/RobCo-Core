/*
 * Minimal host test framework. Include it in one test_*.c file per binary.
 *
 *   TEST(name) { CHECK(x); CHECK_EQ(a, b); CHECK_STR(s, t); CHECK_MEM(p, q, n); }
 *
 * Tests register themselves through a constructor. Checks report file:line
 * and values, then the test goes on. Run with an argument to run only the
 * tests whose name contains it.
 */
#ifndef RC_TEST_H
#define RC_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rc_test {
    const char *name;
    void (*fn)(void);
    struct rc_test *next;
} rc_test_t;

static rc_test_t *rc_tests_head;
static rc_test_t **rc_tests_tail = &rc_tests_head;
static int rc_fails;

static void rc_register(rc_test_t *t)
{
    *rc_tests_tail = t;
    rc_tests_tail = &t->next;
}

/*
 * Returns p, but the compiler cannot see the value. Use it where a test
 * passes something invalid on purpose (a NULL string, a freed or foreign
 * pointer), so GCC does not reject the call at compile time.
 */
static inline void *rc_launder(const void *p)
{
    void *volatile v = (void *)(uintptr_t)p;

    return v;
}

#define TEST(name)                                                        \
    static void rc_test_##name(void);                                     \
    static rc_test_t rc_entry_##name = { #name, rc_test_##name, NULL };   \
    __attribute__((constructor)) static void rc_reg_##name(void)          \
    {                                                                     \
        rc_register(&rc_entry_##name);                                    \
    }                                                                     \
    static void rc_test_##name(void)

#define RC_FAIL(...)                                                      \
    do {                                                                  \
        rc_fails++;                                                       \
        printf("  %s:%d: ", __FILE__, __LINE__);                          \
        printf(__VA_ARGS__);                                              \
        printf("\n");                                                     \
    } while (0)

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            RC_FAIL("CHECK(%s) failed", #cond);                           \
        }                                                                 \
    } while (0)

/* Compares as unsigned 64-bit and prints both values in decimal and hex. */
#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        unsigned long long rc_a = (unsigned long long)(a);                \
        unsigned long long rc_b = (unsigned long long)(b);                \
        if (rc_a != rc_b) {                                               \
            RC_FAIL("CHECK_EQ(%s, %s): %lld (0x%llx) != %lld (0x%llx)",   \
                    #a, #b, (long long)rc_a, rc_a, (long long)rc_b, rc_b); \
        }                                                                 \
    } while (0)

#define CHECK_STR(a, b)                                                   \
    do {                                                                  \
        const char *rc_a = (a);                                           \
        const char *rc_b = (b);                                           \
        if (rc_a == NULL || rc_b == NULL || strcmp(rc_a, rc_b) != 0) {    \
            RC_FAIL("CHECK_STR(%s, %s): \"%s\" != \"%s\"", #a, #b,        \
                    rc_a ? rc_a : "(null)", rc_b ? rc_b : "(null)");      \
        }                                                                 \
    } while (0)

#define CHECK_MEM(a, b, n)                                                \
    do {                                                                  \
        const unsigned char *rc_a = (const unsigned char *)(a);           \
        const unsigned char *rc_b = (const unsigned char *)(b);           \
        size_t rc_n = (n);                                                \
        size_t rc_i;                                                      \
        for (rc_i = 0; rc_i < rc_n; rc_i++) {                             \
            if (rc_a[rc_i] != rc_b[rc_i]) {                               \
                RC_FAIL("CHECK_MEM(%s, %s, %zu): byte %zu: 0x%02x != 0x%02x", \
                        #a, #b, rc_n, rc_i, rc_a[rc_i], rc_b[rc_i]);      \
                break;                                                    \
            }                                                             \
        }                                                                 \
    } while (0)

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    int run = 0;
    int failed = 0;
    rc_test_t *t;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (t = rc_tests_head; t != NULL; t = t->next) {
        int before = rc_fails;

        if (filter != NULL && strstr(t->name, filter) == NULL) {
            continue;
        }
        run++;
        t->fn();
        if (rc_fails != before) {
            failed++;
            printf("FAIL %s\n", t->name);
        } else {
            printf("ok   %s\n", t->name);
        }
    }
    printf("%s: %d tests, %d passed, %d failed\n", argv[0], run, run - failed, failed);
    if (run == 0) {
        printf("no test matches\n");
        return 1;
    }
    return failed ? 1 : 0;
}

#endif
