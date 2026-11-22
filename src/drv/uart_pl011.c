#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/klibc.h>
#include <robco/mbox.h>
#include <robco/mmio.h>
#include <robco/uart.h>

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

static uint32_t uart_base;

void uart_init(uint32_t baud)
{
    const robco_board_t *b = board_get();
    uint32_t clk;
    uint32_t div64;

    if (b->periph_base == 0 || baud == 0) {
        return;
    }
    uart_base = b->periph_base + UART_OFFSET;

    /* The firmware may leave the UART clock unset in the mailbox. */
    if (mbox_get_clock_rate(MBOX_CLOCK_UART, &clk) != 0) {
        clk = UART_FALLBACK_CLOCK;
    }

    /* Disable the UART and let any pending transmit finish. */
    mmio_write(uart_base + UART_CR, 0);
    while (mmio_read(uart_base + UART_FR) & FR_BUSY) {
    }
    mmio_write(uart_base + UART_LCRH, 0);

    /* GPIO14/15 to ALT0. */
    gpio_set_function(14, GPIO_FN_ALT0);
    gpio_set_function(15, GPIO_FN_ALT0);

    /* No pulls on GPIO14/15. gpio_set_pull() refuses BCM2711, which is fine. */
    gpio_set_pull(14, GPIO_PULL_NONE);
    gpio_set_pull(15, GPIO_PULL_NONE);

    /* divisor = clk / (16 * baud), in 1/64 units, rounded. */
    div64 = (clk * 4u + baud / 2u) / baud;

    mmio_write(uart_base + UART_ICR, 0x7ff);
    mmio_write(uart_base + UART_IBRD, div64 >> 6);
    mmio_write(uart_base + UART_FBRD, div64 & 0x3f);
    mmio_write(uart_base + UART_LCRH, LCRH_WLEN8 | LCRH_FEN);
    mmio_write(uart_base + UART_IMSC, 0);
    mmio_write(uart_base + UART_CR, CR_UARTEN | CR_TXE | CR_RXE);

    k_console_set_default(uart_putc);
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

int uart_poll(void)
{
    if (uart_base == 0 || (mmio_read(uart_base + UART_FR) & FR_RXFE)) {
        return -1;
    }
    return (int)(mmio_read(uart_base + UART_DR) & 0xff);
}

int uart_getc(void)
{
    int c;

    while ((c = uart_poll()) < 0) {
    }
    return c;
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

void uart_flush(void)
{
    if (uart_base == 0) {
        return;
    }
    while (mmio_read(uart_base + UART_FR) & FR_BUSY) {
    }
}
