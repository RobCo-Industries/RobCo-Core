#ifndef ROBCO_GPIOX_H
#define ROBCO_GPIOX_H

#include <stddef.h>
#include <stdint.h>

/*
 * PCA9554 and PCA9554A 8-bit I2C GPIO expanders, through i2c.h. Needs
 * i2c_init(). Up to GPIOX_MAX_CHIPS chips; the caller picks a table index
 * for each with gpiox_attach(). No dynamic allocation.
 *
 * The driver keeps a shadow copy of each chip's output and configuration
 * registers. A pin change is one register write built from the shadow, so
 * other pins keep their state. The shadow changes only after the chip
 * acknowledges a write. Levels are pin levels: attach clears the polarity
 * inversion register.
 *
 * Not done: the chip's interrupt output, the 16-bit parts.
 */

#define GPIOX_MAX_CHIPS     4u
#define GPIOX_PINS          8u

/* PCA9554 registers. A configuration bit of 1 makes the pin an input. */
#define GPIOX_REG_INPUT     0u
#define GPIOX_REG_OUTPUT    1u
#define GPIOX_REG_POLARITY  2u
#define GPIOX_REG_CONFIG    3u

/* PCA9554 at 0x20 to 0x27, PCA9554A at 0x38 to 0x3F. */
#define GPIOX_ADDR_MIN      0x20u
#define GPIOX_ADDR_MAX      0x27u
#define GPIOX_ADDR_A_MIN    0x38u
#define GPIOX_ADDR_A_MAX    0x3fu

#define GPIOX_IN            0
#define GPIOX_OUT           1

/* Size of the buffer gpiox_pin_name() needs for any valid pin. */
#define GPIOX_NAME_SIZE     6u

#define GPIOX_ERR_ARG       (-30)   /* bad chip index, pin, direction, level or NULL */
#define GPIOX_ERR_ADDR      (-31)   /* address outside both PCA9554 ranges */
#define GPIOX_ERR_ABSENT    (-32)   /* the chip did not acknowledge the probe */
#define GPIOX_ERR_BUS       (-33)   /* any other I2C failure, including no i2c_init() */
#define GPIOX_ERR_BUSY      (-34)   /* the chip index is already attached */
#define GPIOX_ERR_DETACHED  (-35)   /* the chip index is not attached */
#define GPIOX_ERR_DIR       (-36)   /* a write to a pin that is not an output */

/*
 * Probes `addr` with a read of the configuration register, then reads the
 * output and polarity registers into the shadow. Writes the polarity
 * register to 0 only when it is not 0. Never writes the output or
 * configuration registers, so pins keep their state.
 */
int gpiox_attach(unsigned chip, uint8_t addr);

/* Forgets the chip. Leaves its pins as they are. 0 or an error. */
int gpiox_detach(unsigned chip);

/* 1 when `chip` is attached, else 0. */
int gpiox_present(unsigned chip);

/*
 * Makes a pin an input (GPIOX_IN, `level` ignored) or an output
 * (GPIOX_OUT) at `level` 0 or 1. For an output it writes the output
 * register first and the configuration register second, so the pin never
 * drives the wrong level. Skips a write that would change nothing.
 */
int gpiox_config(unsigned chip, unsigned pin, int dir, int level);

/* Sets an output pin. GPIOX_ERR_DIR for an input. */
int gpiox_write(unsigned chip, unsigned pin, int level);

/* Sets the output pins in `mask` to `bits` in one bus write. GPIOX_ERR_DIR if
 * `mask` holds an input. */
int gpiox_write_port(unsigned chip, uint8_t mask, uint8_t bits);

/* Level of a pin from the input port register; for an output, the level
 * the chip reports on the pin. */
int gpiox_read(unsigned chip, unsigned pin, int *level);

/* The input port register: one bit per pin. */
int gpiox_read_port(unsigned chip, uint8_t *bits);

/* The shadow copies, for diagnostics. 0 or an error. */
int gpiox_shadow(unsigned chip, uint8_t *output, uint8_t *config);

/* Short text for a GPIOX_ERR_* code. */
const char *gpiox_strerror(int code);

/* ---- Pure helpers, src/lib/gpiox_shadow.c ---- */

/* 1 when `addr` is in one of the two address ranges, else 0. */
int gpiox_addr_valid(unsigned addr);

/* 1 when `chip` is a table index below GPIOX_MAX_CHIPS and `pin` is below
 * GPIOX_PINS, else 0. Does not check that the chip is attached. */
int gpiox_pin_valid(unsigned chip, unsigned pin);

/* Writes "X<chip>.<pin>", for example "X0.3". Returns the length or
 * GPIOX_ERR_ARG for an invalid pin, NULL or a buffer too small. */
int gpiox_pin_name(unsigned chip, unsigned pin, char *buf, size_t size);

typedef struct {
    uint8_t output;     /* output register as last acknowledged */
    uint8_t config;     /* configuration register as last acknowledged */
} gpiox_regs_t;

/* Register writes to do, in order. */
typedef struct {
    unsigned count;
    uint8_t reg[2];
    uint8_t val[2];
} gpiox_plan_t;

/* Plan for gpiox_config(). Returns 0 or GPIOX_ERR_ARG. */
int gpiox_plan_config(const gpiox_regs_t *s, unsigned pin, int dir, int level,
                      gpiox_plan_t *plan);

/* Plan for gpiox_write_port(): always one output register write. Returns 0,
 * GPIOX_ERR_ARG or GPIOX_ERR_DIR. */
int gpiox_plan_write(const gpiox_regs_t *s, uint8_t mask, uint8_t bits, gpiox_plan_t *plan);

/* Records a write the chip acknowledged. Ignores other registers. */
void gpiox_regs_apply(gpiox_regs_t *s, uint8_t reg, uint8_t val);

#endif
