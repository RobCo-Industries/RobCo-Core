#include <stdint.h>

#include <robco/gpiox.h>
#include <robco/i2c.h>
#include <robco/klibc.h>
#include <robco/timer.h>
#include <robco/uart.h>

/*
 * Two chips: 0 at 0x20, 1 at 0x21. Nothing answers at 0x22.
 * Phase 0: chip 0 pins 0-3 are outputs and 4-7 inputs, chip 1 the other
 * way round. Phase 1 swaps every direction, so each pin runs both ways.
 * At each step the example writes the outputs, prints "wait" and reads a
 * byte from the serial port; the test checks the outputs and drives the
 * inputs in between. Then it reads the input ports.
 */

#define CHIPS   2u
#define STEPS   4u

static const uint8_t addrs[CHIPS] = { 0x20, 0x21 };
static const uint8_t patterns[STEPS] = { 0x0f, 0x00, 0x05, 0x0a };

static void errors(void)
{
    int level;
    int index_rc = gpiox_attach(GPIOX_MAX_CHIPS, 0x20);
    int addr_rc = gpiox_attach(3, 0x40);
    int absent_rc = gpiox_attach(3, 0x22);
    int detached_rc = gpiox_read(3, 0, &level);
    int name_rc = gpiox_pin_name(0, 8, NULL, 0);

    /* One call per statement: argument order is unspecified in C. */
    k_printf("@RC stage=gpiox ev=errors index=%d addr=%d absent=%d detached=%d name=%d\n",
             index_rc, addr_rc, absent_rc, detached_rc, name_rc);
}

/* Output mask of `chip` in `phase`. */
static uint8_t out_mask(unsigned chip, unsigned phase)
{
    return ((chip ^ phase) & 1u) ? 0xf0u : 0x0fu;
}

/* Spreads a 4-bit pattern over the 4 pins of `mask`. */
static uint8_t spread(uint8_t pat, uint8_t mask)
{
    return mask == 0x0fu ? pat : (uint8_t)(pat << 4);
}

static void configure(unsigned phase)
{
    unsigned chip;
    unsigned pin;

    for (chip = 0; chip < CHIPS; chip++) {
        uint8_t mask = out_mask(chip, phase);
        uint8_t out = 0xff;
        uint8_t cfg = 0xff;
        int rc = 0;

        for (pin = 0; pin < GPIOX_PINS && rc == 0; pin++) {
            if (mask & (1u << pin)) {
                rc = gpiox_config(chip, pin, GPIOX_OUT, 1);
            } else {
                rc = gpiox_config(chip, pin, GPIOX_IN, 0);
            }
        }
        gpiox_shadow(chip, &out, &cfg);
        k_printf("@RC stage=gpiox ev=config phase=%u chip=%u rc=%d out=0x%02x cfg=0x%02x\n",
                 phase, chip, rc, out, cfg);
    }
}

static void step(unsigned phase, unsigned k)
{
    unsigned chip;

    for (chip = 0; chip < CHIPS; chip++) {
        uint8_t mask = out_mask(chip, phase);
        uint8_t bits = 0;
        int wrc = gpiox_write_port(chip, mask, spread(patterns[k], mask));
        int rrc = gpiox_read_port(chip, &bits);

        k_printf("@RC stage=gpiox ev=out phase=%u step=%u chip=%u rc=%d rrc=%d read=0x%02x\n",
                 phase, k, chip, wrc, rrc, bits);
    }
    k_printf("@RC stage=gpiox ev=wait phase=%u step=%u\n", phase, k);
    (void)uart_getc();
    for (chip = 0; chip < CHIPS; chip++) {
        uint8_t bits = 0;
        int rc = gpiox_read_port(chip, &bits);
        int levels = 0;
        unsigned pin;

        /* gpiox_read() per pin must agree with the port read. */
        for (pin = 0; pin < GPIOX_PINS; pin++) {
            int level = -1;

            if (gpiox_read(chip, pin, &level) == 0 && level == 1) {
                levels |= 1 << pin;
            }
        }
        k_printf("@RC stage=gpiox ev=in phase=%u step=%u chip=%u rc=%d read=0x%02x pins=0x%02x\n",
                 phase, k, chip, rc, bits, (unsigned)levels);
    }
}

void robco_main(void)
{
    char name[GPIOX_NAME_SIZE];
    unsigned chip;
    unsigned phase;
    unsigned k;
    int attached = 0;
    int present;
    int wrc;
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=gpiox ev=boot\n");
    rc = i2c_init(100000);
    k_printf("@RC stage=gpiox ev=i2c rc=%d\n", rc);
    errors();

    for (chip = 0; chip < CHIPS; chip++) {
        rc = gpiox_attach(chip, addrs[chip]);
        attached += gpiox_present(chip);
        gpiox_pin_name(chip, 7, name, sizeof(name));
        k_printf("@RC stage=gpiox ev=attach chip=%u addr=0x%02x rc=%d present=%d last=%s\n",
                 chip, addrs[chip], rc, gpiox_present(chip), name);
    }
    rc = gpiox_attach(2, 0x22);
    k_printf("@RC stage=gpiox ev=absent chip=2 addr=0x22 rc=%d present=%d str=\"%s\"\n",
             rc, gpiox_present(2), gpiox_strerror(rc));

    if (attached == (int)CHIPS) {
        k_printf("@RC stage=gpiox ev=busy rc=%d\n", gpiox_attach(0, 0x20));
        for (phase = 0; phase < 2; phase++) {
            configure(phase);
            for (k = 0; k < STEPS; k++) {
                step(phase, k);
            }
        }
        rc = gpiox_detach(1);
        present = gpiox_present(1);
        wrc = gpiox_write(1, 0, 1);
        k_printf("@RC stage=gpiox ev=detach rc=%d present=%d write=%d\n", rc, present, wrc);
    }
    k_printf("@RC stage=gpiox ev=done\n");
    for (;;) {
        timer_delay_ms(1000);
    }
}
