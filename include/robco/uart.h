#ifndef ROBCO_UART_H
#define ROBCO_UART_H

#include <stdint.h>

#define UART_FALLBACK_CLOCK 48000000u

/* PL011 on GPIO14/15, 8N1. Also becomes the k_printf console if none is set. */
void uart_init(uint32_t baud);

void uart_putc(char c);         /* '\n' goes out as "\r\n" */
int uart_getc(void);            /* blocking */
int uart_poll(void);            /* -1 when nothing is pending */
void uart_puts(const char *s);
void uart_flush(void);          /* waits until the transmitter is idle */

/* Reference clock and baud rate in use. Returns 0, or -1 before uart_init(). */
int uart_get_config(uint32_t *clock, uint32_t *baud);

#endif
