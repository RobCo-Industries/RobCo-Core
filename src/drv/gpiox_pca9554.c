/* PCA9554 and PCA9554A I2C GPIO expanders. The shadow logic is in src/lib/gpiox_shadow.c. */
#include <robco/gpiox.h>
#include <robco/i2c.h>

typedef struct {
    uint8_t attached;
    uint8_t addr;
    gpiox_regs_t regs;
} gpiox_chip_t;

static gpiox_chip_t chips[GPIOX_MAX_CHIPS];

static int bus_err(int rc)
{
    return rc < 0 ? GPIOX_ERR_BUS : 0;
}

static int reg_read(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return i2c_write_read(addr, &reg, 1, val, 1);
}

static int reg_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2];

    buf[0] = reg;
    buf[1] = val;
    return i2c_write(addr, buf, sizeof(buf));
}

static gpiox_chip_t *get(unsigned chip, int *rc)
{
    if (chip >= GPIOX_MAX_CHIPS) {
        *rc = GPIOX_ERR_ARG;
        return NULL;
    }
    if (!chips[chip].attached) {
        *rc = GPIOX_ERR_DETACHED;
        return NULL;
    }
    *rc = 0;
    return &chips[chip];
}

/* Each write updates the shadow only after the chip acknowledges it. */
static int run(gpiox_chip_t *c, const gpiox_plan_t *plan)
{
    unsigned i;

    for (i = 0; i < plan->count; i++) {
        if (reg_write(c->addr, plan->reg[i], plan->val[i]) < 0) {
            return GPIOX_ERR_BUS;
        }
        gpiox_regs_apply(&c->regs, plan->reg[i], plan->val[i]);
    }
    return 0;
}

int gpiox_attach(unsigned chip, uint8_t addr)
{
    gpiox_regs_t regs;
    uint8_t pol;
    unsigned i;
    int rc;

    if (chip >= GPIOX_MAX_CHIPS) {
        return GPIOX_ERR_ARG;
    }
    if (!gpiox_addr_valid(addr)) {
        return GPIOX_ERR_ADDR;
    }
    if (chips[chip].attached) {
        return GPIOX_ERR_BUSY;
    }
    for (i = 0; i < GPIOX_MAX_CHIPS; i++) {
        if (chips[i].attached && chips[i].addr == addr) {
            return GPIOX_ERR_BUSY;
        }
    }
    /* The probe: a configuration register read the chip must acknowledge. */
    rc = reg_read(addr, GPIOX_REG_CONFIG, &regs.config);
    if (rc == I2C_ERR_NACK) {
        return GPIOX_ERR_ABSENT;
    }
    if (rc < 0) {
        return GPIOX_ERR_BUS;
    }
    if (reg_read(addr, GPIOX_REG_OUTPUT, &regs.output) < 0 ||
        reg_read(addr, GPIOX_REG_POLARITY, &pol) < 0) {
        return GPIOX_ERR_BUS;
    }
    /* Levels mean pin levels: no inversion. Written only when set, so a
     * chip already in use sees no write at all. */
    if (pol != 0 && reg_write(addr, GPIOX_REG_POLARITY, 0) < 0) {
        return GPIOX_ERR_BUS;
    }
    chips[chip].addr = addr;
    chips[chip].regs = regs;
    chips[chip].attached = 1;
    return 0;
}

int gpiox_detach(unsigned chip)
{
    int rc;
    gpiox_chip_t *c = get(chip, &rc);

    if (c == NULL) {
        return rc;
    }
    c->attached = 0;
    return 0;
}

int gpiox_present(unsigned chip)
{
    return chip < GPIOX_MAX_CHIPS && chips[chip].attached;
}

int gpiox_config(unsigned chip, unsigned pin, int dir, int level)
{
    gpiox_plan_t plan;
    int rc;
    gpiox_chip_t *c = get(chip, &rc);

    if (c == NULL) {
        return rc;
    }
    rc = gpiox_plan_config(&c->regs, pin, dir, level, &plan);
    return rc < 0 ? rc : run(c, &plan);
}

int gpiox_write(unsigned chip, unsigned pin, int level)
{
    int rc;

    if (get(chip, &rc) == NULL) {
        return rc;
    }
    if (pin >= GPIOX_PINS || (level != 0 && level != 1)) {
        return GPIOX_ERR_ARG;
    }
    return gpiox_write_port(chip, (uint8_t)(1u << pin), level ? 0xffu : 0x00u);
}

int gpiox_write_port(unsigned chip, uint8_t mask, uint8_t bits)
{
    gpiox_plan_t plan;
    int rc;
    gpiox_chip_t *c = get(chip, &rc);

    if (c == NULL) {
        return rc;
    }
    rc = gpiox_plan_write(&c->regs, mask, bits, &plan);
    return rc < 0 ? rc : run(c, &plan);
}

int gpiox_read_port(unsigned chip, uint8_t *bits)
{
    int rc;
    gpiox_chip_t *c = get(chip, &rc);

    if (c == NULL) {
        return rc;
    }
    if (bits == NULL) {
        return GPIOX_ERR_ARG;
    }
    return bus_err(reg_read(c->addr, GPIOX_REG_INPUT, bits));
}

int gpiox_read(unsigned chip, unsigned pin, int *level)
{
    uint8_t bits;
    int rc;

    if (get(chip, &rc) == NULL) {
        return rc;
    }
    if (pin >= GPIOX_PINS || level == NULL) {
        return GPIOX_ERR_ARG;
    }
    rc = gpiox_read_port(chip, &bits);
    if (rc == 0) {
        *level = (bits >> pin) & 1;
    }
    return rc;
}

int gpiox_shadow(unsigned chip, uint8_t *output, uint8_t *config)
{
    int rc;
    gpiox_chip_t *c = get(chip, &rc);

    if (c == NULL) {
        return rc;
    }
    if (output == NULL || config == NULL) {
        return GPIOX_ERR_ARG;
    }
    *output = c->regs.output;
    *config = c->regs.config;
    return 0;
}

const char *gpiox_strerror(int code)
{
    switch (code) {
    case 0:                     return "ok";
    case GPIOX_ERR_ARG:         return "bad argument";
    case GPIOX_ERR_ADDR:        return "not a PCA9554 address";
    case GPIOX_ERR_ABSENT:      return "no chip at this address";
    case GPIOX_ERR_BUS:         return "I2C bus error";
    case GPIOX_ERR_BUSY:        return "already attached";
    case GPIOX_ERR_DETACHED:    return "not attached";
    case GPIOX_ERR_DIR:         return "pin is not an output";
    default:                    return "unknown error";
    }
}
