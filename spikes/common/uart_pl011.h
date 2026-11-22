#ifndef SPIKES_UART_PL011_H
#define SPIKES_UART_PL011_H

#include <stdint.h>

#define UART_FALLBACK_CLOCK 48000000u

/* Muxes GPIO14/15 to ALT0 and sets 115200 8N1 from `clock_hz`. */
void uart_init(uintptr_t periph_base, uint32_t clock_hz, int legacy_gpio_pulls);

void uart_putc(char c);
int uart_getc(void);
void uart_puts(const char *s);

#endif
