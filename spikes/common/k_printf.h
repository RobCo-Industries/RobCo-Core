#ifndef SPIKES_K_PRINTF_H
#define SPIKES_K_PRINTF_H

/*
 * Minimal printf over the UART.
 * Supports %s %c %d %u %x, a '0' flag and a field width (e.g. %08x), and %%.
 */
void k_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
