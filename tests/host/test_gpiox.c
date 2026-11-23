/* PCA9554 driver against a fake I2C bus that models the four registers. */
#include <stdint.h>

#include <robco/gpiox.h>
#include <robco/i2c.h>

#include "rc_test.h"

#include "../../src/drv/gpiox_pca9554.c"

#define MAX_LOG 64

typedef struct {
    uint8_t addr;
    uint8_t regs[4];    /* input is computed on read */
    uint8_t ext;        /* levels outside drives on input pins */
} fake_chip_t;

typedef struct {
    int write;          /* 1 for a register write, 0 for a read */
    uint8_t addr;
    uint8_t reg;
    uint8_t val;
} xfer_t;

static fake_chip_t fake[8];
static unsigned nfake;
static xfer_t xlog[MAX_LOG];
static unsigned nlog;
static unsigned xfers;          /* transfers seen, failed ones included */
static unsigned fail_at;        /* 1-based transfer number to fail, 0 for none */
static int fail_rc;

static fake_chip_t *add_chip(uint8_t addr)
{
    fake_chip_t *c = &fake[nfake++];

    c->addr = addr;
    c->regs[GPIOX_REG_INPUT] = 0;
    c->regs[GPIOX_REG_OUTPUT] = 0xff;
    c->regs[GPIOX_REG_POLARITY] = 0x00;
    c->regs[GPIOX_REG_CONFIG] = 0xff;
    c->ext = 0xff;
    return c;
}

static void reset(void)
{
    unsigned i;

    for (i = 0; i < GPIOX_MAX_CHIPS; i++) {
        chips[i].attached = 0;
    }
    nfake = nlog = xfers = fail_at = 0;
    fail_rc = I2C_ERR_TIMEOUT;
}

static void clear_log(void)
{
    nlog = 0;
}

static fake_chip_t *find(uint8_t addr)
{
    unsigned i;

    for (i = 0; i < nfake; i++) {
        if (fake[i].addr == addr) {
            return &fake[i];
        }
    }
    return NULL;
}

static uint8_t pins(const fake_chip_t *c)
{
    uint8_t cfg = c->regs[GPIOX_REG_CONFIG];

    return (uint8_t)((c->ext & cfg) | (c->regs[GPIOX_REG_OUTPUT] & ~cfg));
}

static int fail_now(void)
{
    xfers++;
    return fail_at != 0 && xfers == fail_at;
}

int i2c_write(uint8_t addr, const void *buf, size_t len)
{
    const uint8_t *b = buf;
    fake_chip_t *c;

    if (fail_now()) {
        return fail_rc;
    }
    c = find(addr);
    if (c == NULL) {
        return I2C_ERR_NACK;
    }
    if (len != 2 || b[0] > 3) {
        RC_FAIL("unexpected write len %zu reg %u", len, b[0]);
        return I2C_ERR_ARG;
    }
    if (nlog < MAX_LOG) {
        xlog[nlog++] = (xfer_t){ 1, addr, b[0], b[1] };
    }
    if (b[0] != GPIOX_REG_INPUT) {
        c->regs[b[0]] = b[1];
    }
    return 0;
}

int i2c_write_read(uint8_t addr, const void *wbuf, size_t wlen, void *rbuf, size_t rlen)
{
    uint8_t reg = *(const uint8_t *)wbuf;
    uint8_t *out = rbuf;
    fake_chip_t *c;

    if (fail_now()) {
        return fail_rc;
    }
    c = find(addr);
    if (c == NULL) {
        return I2C_ERR_NACK;
    }
    if (wlen != 1 || rlen != 1 || reg > 3) {
        RC_FAIL("unexpected read");
        return I2C_ERR_ARG;
    }
    if (nlog < MAX_LOG) {
        xlog[nlog++] = (xfer_t){ 0, addr, reg, 0 };
    }
    *out = reg == GPIOX_REG_INPUT ? (uint8_t)(pins(c) ^ c->regs[GPIOX_REG_POLARITY])
                                  : c->regs[reg];
    return 0;
}

static unsigned writes(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < nlog; i++) {
        n += (unsigned)xlog[i].write;
    }
    return n;
}

static void check_shadow(unsigned chip, uint8_t out, uint8_t cfg)
{
    uint8_t o = 0;
    uint8_t c = 0;

    CHECK_EQ(gpiox_shadow(chip, &o, &c), 0);
    CHECK_EQ(o, out);
    CHECK_EQ(c, cfg);
}

TEST(attach_probes_and_reads_without_writing)
{
    fake_chip_t *c;

    reset();
    c = add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    CHECK_EQ(nlog, 3);
    CHECK_EQ(xlog[0].write, 0);
    CHECK_EQ(xlog[0].reg, GPIOX_REG_CONFIG);
    CHECK_EQ(xlog[1].reg, GPIOX_REG_OUTPUT);
    CHECK_EQ(xlog[2].reg, GPIOX_REG_POLARITY);
    CHECK_EQ(writes(), 0);
    CHECK_EQ(gpiox_present(0), 1);
    check_shadow(0, 0xff, 0xff);
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0xff);
}

TEST(attach_clears_polarity_once)
{
    fake_chip_t *c;

    reset();
    c = add_chip(0x3a);
    c->regs[GPIOX_REG_POLARITY] = 0x81;
    CHECK_EQ(gpiox_attach(1, 0x3a), 0);
    CHECK_EQ(writes(), 1);
    CHECK_EQ(xlog[3].write, 1);
    CHECK_EQ(xlog[3].reg, GPIOX_REG_POLARITY);
    CHECK_EQ(xlog[3].val, 0);
    CHECK_EQ(c->regs[GPIOX_REG_POLARITY], 0);
}

TEST(attach_keeps_driven_outputs)
{
    fake_chip_t *c;
    int level = -1;

    reset();
    c = add_chip(0x21);
    c->regs[GPIOX_REG_OUTPUT] = 0x35;
    c->regs[GPIOX_REG_CONFIG] = 0xc0;
    CHECK_EQ(gpiox_attach(0, 0x21), 0);
    CHECK_EQ(writes(), 0);
    check_shadow(0, 0x35, 0xc0);
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0x35);
    CHECK_EQ(c->regs[GPIOX_REG_CONFIG], 0xc0);
    /* A later change to one pin keeps the others driven. */
    clear_log();
    CHECK_EQ(gpiox_write(0, 1, 1), 0);
    CHECK_EQ(nlog, 1);
    CHECK_EQ(xlog[0].val, 0x37);
    CHECK_EQ(gpiox_read(0, 2, &level), 0);
    CHECK_EQ(level, 1);
}

TEST(attach_errors)
{
    reset();
    add_chip(0x20);
    CHECK_EQ(gpiox_attach(GPIOX_MAX_CHIPS, 0x20), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_attach(0, 0x1f), GPIOX_ERR_ADDR);
    CHECK_EQ(gpiox_attach(0, 0x28), GPIOX_ERR_ADDR);
    CHECK_EQ(gpiox_attach(0, 0x40), GPIOX_ERR_ADDR);
    CHECK_EQ(gpiox_attach(0, 0x68), GPIOX_ERR_ADDR);
    CHECK_EQ(gpiox_attach(0, 0x22), GPIOX_ERR_ABSENT);
    CHECK_EQ(gpiox_present(0), 0);
    fail_at = xfers + 1;
    CHECK_EQ(gpiox_attach(0, 0x20), GPIOX_ERR_BUS);
    CHECK_EQ(gpiox_present(0), 0);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    CHECK_EQ(gpiox_attach(0, 0x20), GPIOX_ERR_BUSY);
    CHECK_EQ(gpiox_attach(1, 0x20), GPIOX_ERR_BUSY);
}

TEST(attach_fails_at_each_step)
{
    unsigned step;

    for (step = 1; step <= 4; step++) {
        fake_chip_t *c;

        reset();
        c = add_chip(0x20);
        c->regs[GPIOX_REG_POLARITY] = 0x10;
        fail_at = step;
        CHECK_EQ(gpiox_attach(0, 0x20), GPIOX_ERR_BUS);
        CHECK_EQ(gpiox_present(0), 0);
        CHECK_EQ(gpiox_attach(0, 0x20), 0);
    }
}

TEST(calls_on_a_chip_not_attached)
{
    uint8_t b;
    int level;

    reset();
    add_chip(0x20);
    CHECK_EQ(gpiox_config(0, 0, GPIOX_OUT, 0), GPIOX_ERR_DETACHED);
    CHECK_EQ(gpiox_write(0, 0, 0), GPIOX_ERR_DETACHED);
    CHECK_EQ(gpiox_write_port(0, 1, 1), GPIOX_ERR_DETACHED);
    CHECK_EQ(gpiox_read(0, 0, &level), GPIOX_ERR_DETACHED);
    CHECK_EQ(gpiox_read_port(0, &b), GPIOX_ERR_DETACHED);
    CHECK_EQ(gpiox_detach(0), GPIOX_ERR_DETACHED);
    CHECK_EQ(gpiox_detach(9), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_config(9, 0, GPIOX_OUT, 0), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_present(9), 0);
    CHECK_EQ(nlog, 0);
}

TEST(bad_pin_level_and_null)
{
    int level;

    reset();
    add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    clear_log();
    CHECK_EQ(gpiox_config(0, 8, GPIOX_OUT, 0), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_config(0, 0, 3, 0), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_config(0, 0, GPIOX_OUT, 2), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_write(0, 8, 0), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_write(0, 0, 5), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_read(0, 8, &level), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_read(0, 0, rc_launder(NULL)), GPIOX_ERR_ARG);
    CHECK_EQ(gpiox_read_port(0, rc_launder(NULL)), GPIOX_ERR_ARG);
    CHECK_EQ(nlog, 0);
}

TEST(config_output_writes_level_first)
{
    unsigned pin;

    for (pin = 0; pin < GPIOX_PINS; pin++) {
        fake_chip_t *c;
        uint8_t bit = (uint8_t)(1u << pin);

        reset();
        c = add_chip(0x20);
        CHECK_EQ(gpiox_attach(0, 0x20), 0);
        clear_log();
        CHECK_EQ(gpiox_config(0, pin, GPIOX_OUT, 0), 0);
        CHECK_EQ(nlog, 2);
        CHECK_EQ(xlog[0].reg, GPIOX_REG_OUTPUT);
        CHECK_EQ(xlog[0].val, 0xff & ~bit);
        CHECK_EQ(xlog[1].reg, GPIOX_REG_CONFIG);
        CHECK_EQ(xlog[1].val, 0xff & ~bit);
        CHECK_EQ(pins(c), 0xff & ~bit);
        check_shadow(0, 0xff & ~bit, 0xff & ~bit);
    }
}

TEST(write_on_an_input_is_refused)
{
    reset();
    add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    clear_log();
    CHECK_EQ(gpiox_write(0, 3, 0), GPIOX_ERR_DIR);
    CHECK_EQ(gpiox_write_port(0, 0x08, 0), GPIOX_ERR_DIR);
    CHECK_EQ(nlog, 0);
}

TEST(single_pin_changes_leave_other_pins)
{
    fake_chip_t *c;
    unsigned pin;

    reset();
    c = add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    for (pin = 0; pin < GPIOX_PINS; pin++) {
        CHECK_EQ(gpiox_config(0, pin, GPIOX_OUT, (int)(pin & 1)), 0);
    }
    CHECK_EQ(c->regs[GPIOX_REG_CONFIG], 0x00);
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0xaa);
    for (pin = 0; pin < GPIOX_PINS; pin++) {
        uint8_t bit = (uint8_t)(1u << pin);
        uint8_t before = c->regs[GPIOX_REG_OUTPUT];
        int level = !(before & bit);

        clear_log();
        CHECK_EQ(gpiox_write(0, pin, level), 0);
        CHECK_EQ(nlog, 1);
        CHECK_EQ(c->regs[GPIOX_REG_OUTPUT] & ~bit, before & ~bit);
        CHECK_EQ(!!(c->regs[GPIOX_REG_OUTPUT] & bit), level);
        CHECK_EQ(c->regs[GPIOX_REG_CONFIG], 0x00);
    }
    for (pin = 0; pin < GPIOX_PINS; pin++) {
        uint8_t before = c->regs[GPIOX_REG_CONFIG];

        CHECK_EQ(gpiox_config(0, pin, GPIOX_IN, 0), 0);
        CHECK_EQ(c->regs[GPIOX_REG_CONFIG], before | (1u << pin));
    }
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0x55);
}

TEST(port_write_is_one_transfer)
{
    fake_chip_t *c;

    reset();
    c = add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    CHECK_EQ(gpiox_config(0, 0, GPIOX_OUT, 0), 0);
    CHECK_EQ(gpiox_config(0, 1, GPIOX_OUT, 0), 0);
    CHECK_EQ(gpiox_config(0, 2, GPIOX_OUT, 0), 0);
    clear_log();
    CHECK_EQ(gpiox_write_port(0, 0x07, 0x05), 0);
    CHECK_EQ(nlog, 1);
    CHECK_EQ(xlog[0].reg, GPIOX_REG_OUTPUT);
    CHECK_EQ(xlog[0].val, 0xfd);
    CHECK_EQ(pins(c), 0xfd);
}

TEST(reads_of_inputs_and_outputs)
{
    fake_chip_t *c;
    uint8_t bits = 0;
    int level = -1;

    reset();
    c = add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    CHECK_EQ(gpiox_config(0, 0, GPIOX_OUT, 0), 0);
    CHECK_EQ(gpiox_config(0, 1, GPIOX_OUT, 1), 0);
    c->ext = 0x5c;          /* pins 0 and 1 are outputs: ignored there */
    clear_log();
    CHECK_EQ(gpiox_read_port(0, &bits), 0);
    CHECK_EQ(bits, 0x5e);
    CHECK_EQ(nlog, 1);
    CHECK_EQ(xlog[0].write, 0);
    CHECK_EQ(xlog[0].reg, GPIOX_REG_INPUT);
    CHECK_EQ(gpiox_read(0, 0, &level), 0);
    CHECK_EQ(level, 0);
    CHECK_EQ(gpiox_read(0, 1, &level), 0);
    CHECK_EQ(level, 1);
    CHECK_EQ(gpiox_read(0, 2, &level), 0);
    CHECK_EQ(level, 1);
    CHECK_EQ(gpiox_read(0, 5, &level), 0);
    CHECK_EQ(level, 0);
    fail_at = xfers + 1;
    level = 7;
    CHECK_EQ(gpiox_read(0, 2, &level), GPIOX_ERR_BUS);
    CHECK_EQ(level, 7);
}

/* The fake fails the Nth transfer: the shadow keeps what the chip acknowledged. */
TEST(bus_failure_leaves_shadow_as_acknowledged)
{
    fake_chip_t *c;

    /* Config, first write fails: nothing changes. */
    reset();
    c = add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    fail_at = xfers + 1;
    CHECK_EQ(gpiox_config(0, 4, GPIOX_OUT, 0), GPIOX_ERR_BUS);
    check_shadow(0, 0xff, 0xff);
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0xff);

    /* Config, second write fails: the output write was acknowledged. */
    fail_at = xfers + 2;
    CHECK_EQ(gpiox_config(0, 4, GPIOX_OUT, 0), GPIOX_ERR_BUS);
    check_shadow(0, 0xef, 0xff);
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0xef);
    CHECK_EQ(c->regs[GPIOX_REG_CONFIG], 0xff);

    /* The retry needs only the direction write. */
    clear_log();
    CHECK_EQ(gpiox_config(0, 4, GPIOX_OUT, 0), 0);
    CHECK_EQ(nlog, 1);
    CHECK_EQ(xlog[0].reg, GPIOX_REG_CONFIG);
    check_shadow(0, 0xef, 0xef);

    /* Input config fails. */
    fail_at = xfers + 1;
    CHECK_EQ(gpiox_config(0, 4, GPIOX_IN, 0), GPIOX_ERR_BUS);
    check_shadow(0, 0xef, 0xef);

    /* Pin and port writes fail. */
    fail_at = xfers + 1;
    CHECK_EQ(gpiox_write(0, 4, 1), GPIOX_ERR_BUS);
    check_shadow(0, 0xef, 0xef);
    fail_at = xfers + 1;
    fail_rc = I2C_ERR_NACK;
    CHECK_EQ(gpiox_write_port(0, 0x10, 0x10), GPIOX_ERR_BUS);
    check_shadow(0, 0xef, 0xef);
    CHECK_EQ(c->regs[GPIOX_REG_OUTPUT], 0xef);

    /* And the next write uses the right shadow. */
    CHECK_EQ(gpiox_write(0, 4, 1), 0);
    check_shadow(0, 0xff, 0xef);
}

TEST(four_chips_at_once)
{
    fake_chip_t *c[4];
    static const uint8_t addrs[4] = { 0x20, 0x21, 0x38, 0x3f };
    unsigned i;

    reset();
    for (i = 0; i < 4; i++) {
        c[i] = add_chip(addrs[i]);
        CHECK_EQ(gpiox_attach(i, addrs[i]), 0);
    }
    for (i = 0; i < 4; i++) {
        CHECK_EQ(gpiox_config(i, i, GPIOX_OUT, 0), 0);
    }
    for (i = 0; i < 4; i++) {
        uint8_t bit = (uint8_t)(1u << i);

        CHECK_EQ(c[i]->regs[GPIOX_REG_OUTPUT], 0xff & ~bit);
        CHECK_EQ(c[i]->regs[GPIOX_REG_CONFIG], 0xff & ~bit);
        check_shadow(i, 0xff & ~bit, 0xff & ~bit);
    }
    add_chip(0x22);
    CHECK_EQ(gpiox_attach(4, 0x22), GPIOX_ERR_ARG);
}

TEST(detach_and_attach_again)
{
    fake_chip_t *c;

    reset();
    c = add_chip(0x20);
    CHECK_EQ(gpiox_attach(0, 0x20), 0);
    CHECK_EQ(gpiox_config(0, 6, GPIOX_OUT, 0), 0);
    CHECK_EQ(gpiox_detach(0), 0);
    CHECK_EQ(gpiox_present(0), 0);
    CHECK_EQ(gpiox_write(0, 6, 1), GPIOX_ERR_DETACHED);
    /* Detach leaves the pin driven; attach reads it back. */
    CHECK_EQ(pins(c), 0xbf);
    clear_log();
    CHECK_EQ(gpiox_attach(2, 0x20), 0);
    CHECK_EQ(writes(), 0);
    check_shadow(2, 0xbf, 0xbf);
}

TEST(strerror_names)
{
    CHECK_STR(gpiox_strerror(GPIOX_ERR_ABSENT), "no chip at this address");
    CHECK_STR(gpiox_strerror(GPIOX_ERR_DIR), "pin is not an output");
    CHECK_STR(gpiox_strerror(-999), "unknown error");
}
