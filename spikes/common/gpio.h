#ifndef SPIKES_GPIO_H
#define SPIKES_GPIO_H

#include <stdint.h>

#define GPIO_FSEL_IN   0
#define GPIO_FSEL_OUT  1
#define GPIO_FSEL_ALT0 4
#define GPIO_FSEL_ALT1 5
#define GPIO_FSEL_ALT2 6
#define GPIO_FSEL_ALT3 7
#define GPIO_FSEL_ALT4 3
#define GPIO_FSEL_ALT5 2

void gpio_init(uintptr_t periph_base);

/* Raw 3-bit function select of `pin` (0..53). */
uint32_t gpio_get_fsel(uint32_t pin);
void gpio_set_fsel(uint32_t pin, uint32_t fsel);

#endif
