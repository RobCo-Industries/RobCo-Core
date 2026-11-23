/* PCA9554 shadow logic: which register writes a pin change needs. Pure. */
#include <robco/gpiox.h>
#include <robco/klibc.h>

int gpiox_addr_valid(unsigned addr)
{
    return (addr >= GPIOX_ADDR_MIN && addr <= GPIOX_ADDR_MAX) ||
           (addr >= GPIOX_ADDR_A_MIN && addr <= GPIOX_ADDR_A_MAX);
}

int gpiox_pin_valid(unsigned chip, unsigned pin)
{
    return chip < GPIOX_MAX_CHIPS && pin < GPIOX_PINS;
}

int gpiox_pin_name(unsigned chip, unsigned pin, char *buf, size_t size)
{
    if (!gpiox_pin_valid(chip, pin) || buf == NULL || size < GPIOX_NAME_SIZE) {
        return GPIOX_ERR_ARG;
    }
    return k_snprintf(buf, size, "X%u.%u", chip, pin);
}

static void plan_add(gpiox_plan_t *plan, uint8_t reg, uint8_t val)
{
    plan->reg[plan->count] = reg;
    plan->val[plan->count] = val;
    plan->count++;
}

int gpiox_plan_config(const gpiox_regs_t *s, unsigned pin, int dir, int level,
                      gpiox_plan_t *plan)
{
    uint8_t bit;
    uint8_t out;
    uint8_t cfg;

    if (s == NULL || plan == NULL || pin >= GPIOX_PINS ||
        (dir != GPIOX_IN && dir != GPIOX_OUT) ||
        (dir == GPIOX_OUT && level != 0 && level != 1)) {
        return GPIOX_ERR_ARG;
    }
    plan->count = 0;
    bit = (uint8_t)(1u << pin);
    if (dir == GPIOX_IN) {
        cfg = (uint8_t)(s->config | bit);
        if (cfg != s->config) {
            plan_add(plan, GPIOX_REG_CONFIG, cfg);
        }
        return 0;
    }
    /* Output level first, then direction: the pin never drives the wrong level. */
    out = level ? (uint8_t)(s->output | bit) : (uint8_t)(s->output & ~bit);
    cfg = (uint8_t)(s->config & ~bit);
    if (out != s->output) {
        plan_add(plan, GPIOX_REG_OUTPUT, out);
    }
    if (cfg != s->config) {
        plan_add(plan, GPIOX_REG_CONFIG, cfg);
    }
    return 0;
}

int gpiox_plan_write(const gpiox_regs_t *s, uint8_t mask, uint8_t bits, gpiox_plan_t *plan)
{
    if (s == NULL || plan == NULL) {
        return GPIOX_ERR_ARG;
    }
    if (mask & s->config) {
        return GPIOX_ERR_DIR;
    }
    plan->count = 0;
    plan_add(plan, GPIOX_REG_OUTPUT, (uint8_t)((s->output & ~mask) | (bits & mask)));
    return 0;
}

void gpiox_regs_apply(gpiox_regs_t *s, uint8_t reg, uint8_t val)
{
    if (s == NULL) {
        return;
    }
    if (reg == GPIOX_REG_OUTPUT) {
        s->output = val;
    } else if (reg == GPIOX_REG_CONFIG) {
        s->config = val;
    }
}
