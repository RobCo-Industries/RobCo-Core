#include <stdint.h>

#include <robco/input.h>

#include "rc_test.h"

typedef struct {
    unsigned presses;
    unsigned releases;
    int first_press_at;
    int first_release_at;
} edges_t;

/* Feeds a string of '0'/'1' levels for line 0 and records its edges. */
static edges_t run(input_debounce_t *d, const char *levels)
{
    edges_t e = { 0, 0, -1, -1 };
    uint32_t p, r;
    int i;

    for (i = 0; levels[i]; i++) {
        input_debounce_feed(d, levels[i] == '1' ? 1u : 0u, &p, &r);
        if (p & 1) {
            e.presses++;
            if (e.first_press_at < 0) {
                e.first_press_at = i;
            }
        }
        if (r & 1) {
            e.releases++;
            if (e.first_release_at < 0) {
                e.first_release_at = i;
            }
        }
    }
    return e;
}

TEST(debounce_clean_active_high)
{
    input_debounce_t d;
    edges_t e;

    input_debounce_init(&d, 3, 0);
    e = run(&d, "000111111000000");
    CHECK_EQ(e.presses, 1);
    CHECK_EQ(e.releases, 1);
    CHECK_EQ(e.first_press_at, 5);     /* third high sample */
    CHECK_EQ(e.first_release_at, 11);
}

TEST(debounce_active_low)
{
    input_debounce_t d;
    edges_t e;
    uint32_t p, r;

    input_debounce_init(&d, 2, 1);
    /* Idle high is released: no edge. */
    e = run(&d, "1111");
    CHECK_EQ(e.presses + e.releases, 0);
    e = run(&d, "00001111");
    CHECK_EQ(e.presses, 1);
    CHECK_EQ(e.releases, 1);
    CHECK_EQ(input_debounce_feed(&d, 1, &p, &r) & 1, 0);
}

TEST(debounce_bouncy_press_and_release)
{
    input_debounce_t d;
    edges_t e;

    input_debounce_init(&d, 4, 0);
    e = run(&d, "0001010110111111111011010000000000");
    CHECK_EQ(e.presses, 1);
    CHECK_EQ(e.releases, 1);
}

TEST(debounce_glitch_shorter_than_stable)
{
    input_debounce_t d;
    edges_t e;

    input_debounce_init(&d, 4, 0);
    e = run(&d, "0000111000001110000110000100");
    CHECK_EQ(e.presses, 0);
    CHECK_EQ(e.releases, 0);
    /* While pressed, a short dropout does not release. */
    e = run(&d, "1111111111011101101111111");
    CHECK_EQ(e.presses, 1);
    CHECK_EQ(e.releases, 0);
}

TEST(debounce_stable_one)
{
    input_debounce_t d;
    edges_t e;

    input_debounce_init(&d, 1, 0);
    e = run(&d, "0101");
    CHECK_EQ(e.presses, 2);
    CHECK_EQ(e.releases, 1);
}

TEST(debounce_simultaneous_lines)
{
    input_debounce_t d;
    uint32_t p, r, s = 0;
    uint32_t pressed_seen = 0, released_seen = 0;
    unsigned i;

    /* Lines 0 and 31 active-low, 5 active-high. All three go active together. */
    input_debounce_init(&d, 3, (1u << 0) | (1u << 31));
    for (i = 0; i < 10; i++) {
        s = input_debounce_feed(&d, 0, &p, &r);
        pressed_seen |= p;
        released_seen |= r;
        CHECK_EQ(p & r, 0);
    }
    /* Line 0 and 31 read 0: pressed. Line 5 reads 0: released (no edge). */
    CHECK_EQ(pressed_seen, (1u << 0) | (1u << 31));
    CHECK_EQ(s, (1u << 0) | (1u << 31));
    for (i = 0; i < 10; i++) {
        s = input_debounce_feed(&d, (1u << 0) | (1u << 31) | (1u << 5), &p, &r);
        pressed_seen |= p;
        released_seen |= r;
    }
    CHECK_EQ(s, 1u << 5);
    CHECK_EQ(released_seen, (1u << 0) | (1u << 31));
    CHECK_EQ(pressed_seen, (1u << 0) | (1u << 31) | (1u << 5));
}

TEST(debounce_lines_are_independent)
{
    input_debounce_t d;
    uint32_t p, r;
    unsigned i, n1 = 0, n2 = 0;

    input_debounce_init(&d, 3, 0);
    /* Line 1 bounces all the time, line 2 is held. */
    for (i = 0; i < 20; i++) {
        input_debounce_feed(&d, ((i & 1) << 1) | (1u << 2), &p, &r);
        n1 += (p >> 1) & 1;
        n2 += (p >> 2) & 1;
    }
    CHECK_EQ(n1, 0);
    CHECK_EQ(n2, 1);
}

TEST(debounce_samples)
{
    CHECK_EQ(input_debounce_samples(20, 100), 2);
    CHECK_EQ(input_debounce_samples(25, 100), 3);
    CHECK_EQ(input_debounce_samples(0, 100), 1);
    CHECK_EQ(input_debounce_samples(5, 1000), 5);
    CHECK_EQ(input_debounce_samples(10000, 10000), 255);
    CHECK_EQ(input_debounce_samples(20, 0), 1);
}

TEST(debounce_clamps_stable)
{
    input_debounce_t d;

    input_debounce_init(&d, 0, 0);
    CHECK_EQ(d.stable, 1);
    input_debounce_init(&d, 1000, 0);
    CHECK_EQ(d.stable, 255);
}
