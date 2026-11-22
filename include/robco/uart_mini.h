#ifndef ROBCO_UART_MINI_H
#define ROBCO_UART_MINI_H

#include <stdint.h>

/*
 * Mini UART in the AUX block, 8N1. A test and bring-up channel: in QEMU
 * it is the second -serial. uart_mini_init() does not mux any pin. On a
 * board its pins are GPIO14/15 (ALT5), which the PL011 console uses.
 */

#define UART_MINI_FALLBACK_CORE_CLOCK  250000000u

#define UART_MINI_ERR_BOARD (-1)    /* no peripheral base */
#define UART_MINI_ERR_BAUD  (-2)    /* baud 0 or out of range for the core clock */

/* Mailbox clock id of the VPU core clock, which drives the mini UART. */
#define MBOX_CLOCK_CORE     4

/* Enables the mini UART in AUXENB and sets 8-bit mode and the baud rate. Returns 0 or an error. */
int uart_mini_init(uint32_t baud);

void uart_mini_putc(char c);        /* '\n' goes out as "\r\n" */
int uart_mini_getc(void);           /* blocking; -1 at once before uart_mini_init() */
int uart_mini_poll(void);           /* -1 when nothing is pending */
int uart_mini_ready(void);          /* 1 after uart_mini_init() succeeded, else 0 */
void uart_mini_puts(const char *s);

#endif
