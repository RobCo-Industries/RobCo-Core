#include "gpio.h"
#include "mmio.h"

#define GPIO_OFFSET 0x200000

static uintptr_t gpio_base;

void gpio_init(uintptr_t periph_base)
{
    gpio_base = periph_base + GPIO_OFFSET;
}

uint32_t gpio_get_fsel(uint32_t pin)
{
    uintptr_t reg = gpio_base + (pin / 10) * 4;

    return (mmio_read(reg) >> ((pin % 10) * 3)) & 7;
}

void gpio_set_fsel(uint32_t pin, uint32_t fsel)
{
    uintptr_t reg = gpio_base + (pin / 10) * 4;
    uint32_t shift = (pin % 10) * 3;
    uint32_t v = mmio_read(reg);

    v &= ~(7u << shift);
    v |= (fsel & 7) << shift;
    mmio_write(reg, v);
}
