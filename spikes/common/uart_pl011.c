#include "uart_pl011.h"
#include "mmio.h"

#define GPIO_OFFSET     0x200000
#define GPFSEL1         0x04
#define GPPUD           0x94
#define GPPUDCLK0       0x98

#define UART_OFFSET     0x201000
#define UART_DR         0x00
#define UART_FR         0x18
#define UART_IBRD       0x24
#define UART_FBRD       0x28
#define UART_LCRH       0x2c
#define UART_CR         0x30
#define UART_IMSC       0x38
#define UART_ICR        0x44

#define FR_BUSY         (1u << 3)
#define FR_RXFE         (1u << 4)
#define FR_TXFF         (1u << 5)
#define LCRH_FEN        (1u << 4)
#define LCRH_WLEN8      (3u << 5)
#define CR_UARTEN       (1u << 0)
#define CR_TXE          (1u << 8)
#define CR_RXE          (1u << 9)

#define BAUD            115200u

static uintptr_t uart_base;

void uart_init(uintptr_t periph_base, uint32_t clock_hz, int legacy_gpio_pulls)
{
    uintptr_t gpio = periph_base + GPIO_OFFSET;
    uint32_t sel;
    uint32_t div64;

    uart_base = periph_base + UART_OFFSET;

    /* Disable the UART and let any pending transmit finish. */
    mmio_write(uart_base + UART_CR, 0);
    while (mmio_read(uart_base + UART_FR) & FR_BUSY) {
    }
    mmio_write(uart_base + UART_LCRH, 0);

    /* GPIO14/15 to ALT0 (bits 14:12 and 17:15 of GPFSEL1). */
    sel = mmio_read(gpio + GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (4u << 12) | (4u << 15);
    mmio_write(gpio + GPFSEL1, sel);

    /* No pull-up/down on GPIO14/15 (BCM2835-2837 sequence). */
    if (legacy_gpio_pulls) {
        mmio_write(gpio + GPPUD, 0);
        delay_cycles(150);
        mmio_write(gpio + GPPUDCLK0, (1u << 14) | (1u << 15));
        delay_cycles(150);
        mmio_write(gpio + GPPUD, 0);
        mmio_write(gpio + GPPUDCLK0, 0);
    }

    /* divisor = clock / (16 * baud), in 1/64 units, rounded. */
    div64 = (clock_hz * 4u + BAUD / 2u) / BAUD;

    mmio_write(uart_base + UART_ICR, 0x7ff);
    mmio_write(uart_base + UART_IBRD, div64 >> 6);
    mmio_write(uart_base + UART_FBRD, div64 & 0x3f);
    mmio_write(uart_base + UART_LCRH, LCRH_WLEN8 | LCRH_FEN);
    mmio_write(uart_base + UART_IMSC, 0);
    mmio_write(uart_base + UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

static void uart_put_raw(char c)
{
    while (mmio_read(uart_base + UART_FR) & FR_TXFF) {
    }
    mmio_write(uart_base + UART_DR, (uint32_t)(unsigned char)c);
}

void uart_putc(char c)
{
    if (uart_base == 0) {
        return;
    }
    if (c == '\n') {
        uart_put_raw('\r');
    }
    uart_put_raw(c);
}

int uart_getc(void)
{
    while (mmio_read(uart_base + UART_FR) & FR_RXFE) {
    }
    return (int)(mmio_read(uart_base + UART_DR) & 0xff);
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}
