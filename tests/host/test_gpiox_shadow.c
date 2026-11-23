/* PCA9554 shadow logic: register writes planned for each pin change. */
#include <stdint.h>

#include <robco/gpiox.h>

#include "rc_test.h"

static const gpiox_regs_t reset = { 0xff, 0xff };

TEST(addresses)
{
    unsigned a;
    unsigned n = 0;

    for (a = 0; a < 0x100; a++) {
        n += (unsigned)gpiox_addr_valid(a);
    }
    CHECK_EQ(n, 16);
    CHECK(gpiox_addr_valid(0x20));
    CHECK(gpiox_addr_valid(0x27));
    CHECK(gpiox_addr_valid(0x38));
    CHECK(gpiox_addr_valid(0x3f));
    CHECK(!gpiox_addr_valid(0x1f));
    CHECK(!gpiox_addr_valid(0x28));
    CHECK(!gpiox_addr_valid(0x37));
    CHECK(!gpiox_addr_valid(0x40));
}

TEST(pin_valid_and_name)
{
    char buf[GPIOX_NAME_SIZE];

    CHECK(gpiox_pin_valid(0, 0));
    CHECK(gpiox_pin_valid(3, 7));
    CHECK(!gpiox_pin_valid(4, 0));
    CHECK(!gpiox_pin_valid(0, 8));
    CHECK_EQ(gpiox_pin_name(0, 3, buf, sizeof(buf)), 4);
    CHECK_STR(buf, "X0.3");
    CHECK_EQ(gpiox_pin_name(3, 7, buf, sizeof(buf)), 4);
    CHECK_STR(buf, "X3.7");
    CHECK_EQ(gpiox_pin_name(4, 0, buf, sizeof(buf)), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_pin_name(0, 8, buf, sizeof(buf)), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_pin_name(0, 0, buf, 5), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_pin_name(0, 0, rc_launder(NULL), 8), GPIOX_ERR_ARG);
}

TEST(output_writes_level_before_direction)
{
    gpiox_plan_t p;
    unsigned pin;

    for (pin = 0; pin < GPIOX_PINS; pin++) {
        uint8_t bit = (uint8_t)(1u << pin);

        CHECK_EQ(gpiox_plan_config(&reset, pin, GPIOX_OUT, 0, &p), 0);
        CHECK_EQ(p.count, 2);
        CHECK_EQ(p.reg[0], GPIOX_REG_OUTPUT);
        CHECK_EQ(p.val[0], 0xff & ~bit);
        CHECK_EQ(p.reg[1], GPIOX_REG_CONFIG);
        CHECK_EQ(p.val[1], 0xff & ~bit);
    }
}

TEST(output_high_after_reset_needs_only_direction)
{
    gpiox_plan_t p;

    CHECK_EQ(gpiox_plan_config(&reset, 5, GPIOX_OUT, 1, &p), 0);
    CHECK_EQ(p.count, 1);
    CHECK_EQ(p.reg[0], GPIOX_REG_CONFIG);
    CHECK_EQ(p.val[0], 0xdf);
}

TEST(input_sets_the_config_bit_only)
{
    gpiox_regs_t s = { 0x00, 0x00 };
    gpiox_plan_t p;
    unsigned pin;

    for (pin = 0; pin < GPIOX_PINS; pin++) {
        CHECK_EQ(gpiox_plan_config(&s, pin, GPIOX_IN, 0, &p), 0);
        CHECK_EQ(p.count, 1);
        CHECK_EQ(p.reg[0], GPIOX_REG_CONFIG);
        CHECK_EQ(p.val[0], 1u << pin);
    }
    CHECK_EQ(gpiox_plan_config(&reset, 2, GPIOX_IN, 0, &p), 0);
    CHECK_EQ(p.count, 0);
}

TEST(single_bit_changes_keep_other_bits)
{
    static const uint8_t patterns[] = { 0x00, 0xff, 0xa5, 0x5a, 0x0f };
    gpiox_plan_t p;
    unsigned i;
    unsigned pin;
    int level;

    for (i = 0; i < sizeof(patterns); i++) {
        for (pin = 0; pin < GPIOX_PINS; pin++) {
            for (level = 0; level <= 1; level++) {
                gpiox_regs_t s = { patterns[i], (uint8_t)~patterns[i] };
                uint8_t bit = (uint8_t)(1u << pin);

                CHECK_EQ(gpiox_plan_config(&s, pin, GPIOX_OUT, level, &p), 0);
                for (unsigned k = 0; k < p.count; k++) {
                    gpiox_regs_apply(&s, p.reg[k], p.val[k]);
                }
                CHECK_EQ(s.output & ~bit, patterns[i] & ~bit);
                CHECK_EQ(s.config & ~bit, (uint8_t)~patterns[i] & ~bit);
                CHECK_EQ(!!(s.output & bit), level);
                CHECK_EQ(s.config & bit, 0);
            }
        }
    }
}

TEST(port_write)
{
    gpiox_regs_t s = { 0x0f, 0xf0 };
    gpiox_plan_t p;

    CHECK_EQ(gpiox_plan_write(&s, 0x05, 0x04, &p), 0);
    CHECK_EQ(p.count, 1);
    CHECK_EQ(p.reg[0], GPIOX_REG_OUTPUT);
    CHECK_EQ(p.val[0], 0x0e);
    CHECK_EQ(gpiox_plan_write(&s, 0x10, 0x10, &p), GPIOX_ERR_DIR);
    CHECK_EQ(gpiox_plan_write(&s, 0x00, 0xff, &p), 0);
    CHECK_EQ(p.val[0], 0x0f);
}

TEST(bad_arguments)
{
    gpiox_plan_t p;

    CHECK_EQ(gpiox_plan_config(&reset, 8, GPIOX_OUT, 0, &p), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_plan_config(&reset, 0, 2, 0, &p), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_plan_config(&reset, 0, GPIOX_OUT, 2, &p), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_plan_config(rc_launder(NULL), 0, GPIOX_OUT, 0, &p), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_plan_config(&reset, 0, GPIOX_OUT, 0, rc_launder(NULL)), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_plan_write(rc_launder(NULL), 1, 1, &p), GPIOX_ERR_ARG);
}

TEST(apply_ignores_input_and_polarity)
{
    gpiox_regs_t s = reset;

    gpiox_regs_apply(&s, GPIOX_REG_INPUT, 0);
    gpiox_regs_apply(&s, GPIOX_REG_POLARITY, 0);
    CHECK_EQ(s.output, 0xff);
    CHECK_EQ(s.config, 0xff);
}
