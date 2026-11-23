#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/mmio.h>

#define GPIO_OFFSET     0x200000
#define GPFSEL0         0x00
#define GPSET0          0x1c
#define GPCLR0          0x28
#define GPLEV0          0x34
#define GPPUD           0x94
#define GPPUDCLK0       0x98

/*
 * Datasheet asks for at least 150 cycles between pull steps. 5 us covers
 * 150 cycles of any clock above 30 MHz. uart_init() sets pulls before a
 * stage can print, so the wait must not use the system timer: a stopped
 * timer would hang it there. It counts loop iterations instead. One
 * iteration ends in a taken branch, which takes at least one CPU cycle on
 * every supported core, so PULL_DELAY_LOOPS iterations last at least 5 us
 * up to 1.5 GHz, above the fastest supported clock (1.4 GHz, Pi 3B+). On
 * a slower clock the wait only gets longer: about 11 us at 700 MHz.
 */
#define PULL_DELAY_LOOPS 7500u

static void pull_settle(void)
{
    uint32_t n = PULL_DELAY_LOOPS;

    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc", "memory");
}

static uint32_t gpio_base(void)
{
    const robco_board_t *b = board_get();

    if (b->periph_base == 0) {
        return 0;
    }
    return b->periph_base + GPIO_OFFSET;
}

/* Bank register for a pin: reg0 for pins 0 to 31, reg0 + 4 above. */
static uint32_t bank_reg(uint32_t base, uint32_t reg0, unsigned pin)
{
    return base + reg0 + (pin / 32) * 4;
}

int gpio_set_function(unsigned pin, unsigned fn)
{
    uint32_t base = gpio_base();
    uint32_t reg;
    uint32_t shift;
    uint32_t v;

    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (fn > 7) {
        return GPIO_ERR_ARG;
    }
    if (base == 0) {
        return GPIO_ERR_BOARD;
    }
    reg = base + GPFSEL0 + (pin / 10) * 4;
    shift = (pin % 10) * 3;
    v = mmio_read(reg);
    v &= ~(7u << shift);
    v |= (uint32_t)fn << shift;
    mmio_write(reg, v);
    return 0;
}

int gpio_get_function(unsigned pin)
{
    uint32_t base = gpio_base();

    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (base == 0) {
        return GPIO_ERR_BOARD;
    }
    return (int)((mmio_read(base + GPFSEL0 + (pin / 10) * 4) >> ((pin % 10) * 3)) & 7u);
}

int gpio_set_pull(unsigned pin, unsigned pull)
{
    uint32_t base = gpio_base();
    uint32_t clk;

    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (pull > GPIO_PULL_UP) {
        return GPIO_ERR_ARG;
    }
    if (base == 0 || board_get()->soc == ROBCO_SOC_BCM2711) {
        return GPIO_ERR_BOARD;
    }
    clk = bank_reg(base, GPPUDCLK0, pin);
    mmio_write(base + GPPUD, pull);
    pull_settle();
    mmio_write(clk, 1u << (pin % 32));
    pull_settle();
    mmio_write(base + GPPUD, 0);
    mmio_write(clk, 0);
    return 0;
}

int gpio_write(unsigned pin, int level)
{
    uint32_t base = gpio_base();

    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (base == 0) {
        return GPIO_ERR_BOARD;
    }
    mmio_write(bank_reg(base, level ? GPSET0 : GPCLR0, pin), 1u << (pin % 32));
    return 0;
}

int gpio_read(unsigned pin)
{
    uint32_t base = gpio_base();

    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (base == 0) {
        return GPIO_ERR_BOARD;
    }
    return (int)((mmio_read(bank_reg(base, GPLEV0, pin)) >> (pin % 32)) & 1u);
}

int gpio_toggle(unsigned pin)
{
    int level = gpio_read(pin);

    if (level < 0) {
        return level;
    }
    return gpio_write(pin, !level);
}

int gpio_config_output(unsigned pin, int initial_level)
{
    int rc = gpio_write(pin, initial_level);

    if (rc < 0) {
        return rc;
    }
    return gpio_set_function(pin, GPIO_FN_OUTPUT);
}

int gpio_config_input(unsigned pin, unsigned pull)
{
    int rc;

    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (pull > GPIO_PULL_UP) {
        return GPIO_ERR_ARG;
    }
    rc = gpio_set_function(pin, GPIO_FN_INPUT);
    if (rc < 0) {
        return rc;
    }
    return gpio_set_pull(pin, pull);
}
