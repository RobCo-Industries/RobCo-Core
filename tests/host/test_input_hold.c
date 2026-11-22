#include <stdint.h>

#include <robco/input.h>

#include "rc_test.h"

#define K INPUT_KEY_SELECT

static int ev(input_hold_t *h, unsigned src, unsigned code, int value)
{
    input_event_t e = { INPUT_EV_KEY, (uint8_t)src, (uint16_t)code, value, 0 };

    return input_hold_update(h, &e);
}

TEST(hold_single_source)
{
    input_hold_t h;

    input_hold_init(&h);
    CHECK(!input_hold_down(&h, K));
    CHECK_EQ(ev(&h, INPUT_SRC_GPIO, K, INPUT_PRESS), 1);
    CHECK(input_hold_down(&h, K));
    CHECK_EQ(h.rep_code, K);
    CHECK_EQ(ev(&h, INPUT_SRC_GPIO, K, INPUT_RELEASE), 0);
    CHECK(!input_hold_down(&h, K));
    CHECK_EQ(h.rep_code, 0);
}

/* The bug: a serial tap on a key the GPIO button holds released it. */
TEST(hold_two_sources_tap_over_hold)
{
    input_hold_t h;

    input_hold_init(&h);
    ev(&h, INPUT_SRC_GPIO, K, INPUT_PRESS);
    CHECK_EQ(ev(&h, INPUT_SRC_SERIAL, K, INPUT_PRESS), 1);
    CHECK_EQ(h.rep_source, INPUT_SRC_SERIAL);
    ev(&h, INPUT_SRC_SERIAL, K, INPUT_RELEASE);
    CHECK(input_hold_down(&h, K));
    CHECK_EQ(h.rep_code, K);
    CHECK_EQ(h.rep_source, INPUT_SRC_GPIO);
    ev(&h, INPUT_SRC_GPIO, K, INPUT_RELEASE);
    CHECK(!input_hold_down(&h, K));
    CHECK_EQ(h.rep_code, 0);
}

TEST(hold_two_sources_release_orders)
{
    unsigned order;

    for (order = 0; order < 2; order++) {
        input_hold_t h;
        unsigned first = order ? INPUT_SRC_SCRIPT : INPUT_SRC_SERIAL;
        unsigned second = order ? INPUT_SRC_SERIAL : INPUT_SRC_SCRIPT;

        input_hold_init(&h);
        ev(&h, INPUT_SRC_SERIAL, K, INPUT_PRESS);
        ev(&h, INPUT_SRC_SCRIPT, K, INPUT_PRESS);
        ev(&h, first, K, INPUT_RELEASE);
        CHECK(input_hold_down(&h, K));
        CHECK_EQ(h.rep_code, K);
        CHECK_EQ(h.rep_source, second);
        ev(&h, second, K, INPUT_RELEASE);
        CHECK(!input_hold_down(&h, K));
        CHECK_EQ(h.rep_code, 0);
    }
}

TEST(hold_three_sources_all_permutations)
{
    static const unsigned perm[6][3] = {
        { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 },
    };
    unsigned p, i;

    for (p = 0; p < 6; p++) {
        input_hold_t h;

        input_hold_init(&h);
        for (i = 0; i < 3; i++) {
            ev(&h, i, K, INPUT_PRESS);
        }
        for (i = 0; i < 3; i++) {
            CHECK(input_hold_down(&h, K));
            CHECK_EQ(h.rep_code, K);
            ev(&h, perm[p][i], K, INPUT_RELEASE);
        }
        CHECK(!input_hold_down(&h, K));
        CHECK_EQ(h.rep_code, 0);
    }
}

TEST(hold_release_from_source_that_never_pressed)
{
    input_hold_t h;

    input_hold_init(&h);
    ev(&h, INPUT_SRC_GPIO, K, INPUT_PRESS);
    CHECK_EQ(ev(&h, INPUT_SRC_SERIAL, K, INPUT_RELEASE), 0);
    CHECK(input_hold_down(&h, K));
    CHECK_EQ(h.rep_code, K);
    CHECK_EQ(h.rep_source, INPUT_SRC_GPIO);
    /* Nothing held at all: still a no-op. */
    ev(&h, INPUT_SRC_GPIO, K, INPUT_RELEASE);
    ev(&h, INPUT_SRC_SCRIPT, K, INPUT_RELEASE);
    CHECK(!input_hold_down(&h, K));
    CHECK_EQ(h.rep_code, 0);
}

TEST(hold_repeat_follows_last_press)
{
    input_hold_t h;

    input_hold_init(&h);
    ev(&h, INPUT_SRC_GPIO, INPUT_KEY_UP, INPUT_PRESS);
    ev(&h, INPUT_SRC_SERIAL, INPUT_KEY_DOWN, INPUT_PRESS);
    CHECK_EQ(h.rep_code, INPUT_KEY_DOWN);
    /* Releasing the older key keeps the newer repeat. */
    ev(&h, INPUT_SRC_GPIO, INPUT_KEY_UP, INPUT_RELEASE);
    CHECK_EQ(h.rep_code, INPUT_KEY_DOWN);
    CHECK(!input_hold_down(&h, INPUT_KEY_UP));
    ev(&h, INPUT_SRC_SERIAL, INPUT_KEY_DOWN, INPUT_RELEASE);
    CHECK_EQ(h.rep_code, 0);
}

TEST(hold_ignores_other_events)
{
    input_hold_t h;
    input_event_t e = { INPUT_EV_CHAR, INPUT_SRC_SERIAL, K, INPUT_PRESS, 0 };

    input_hold_init(&h);
    CHECK_EQ(input_hold_update(&h, &e), 0);
    CHECK(!input_hold_down(&h, K));
    CHECK_EQ(ev(&h, INPUT_HOLD_SOURCES, K, INPUT_PRESS), 0);
    CHECK_EQ(ev(&h, INPUT_SRC_GPIO, 40, INPUT_PRESS), 0);
    CHECK(!input_hold_down(&h, 40));
    ev(&h, INPUT_SRC_GPIO, K, INPUT_PRESS);
    CHECK_EQ(ev(&h, INPUT_SRC_GPIO, K, INPUT_REPEAT), 0);
    CHECK(input_hold_down(&h, K));
}
