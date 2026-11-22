#include <stdint.h>

#include <robco/board.h>
#include <robco/mbox.h>
#include <robco/mmio.h>
#include <robco/uart_mini.h>

#define AUX_OFFSET      0x215000
#define AUX_ENABLES     0x04
#define AUX_MU_IO       0x40
#define AUX_MU_IER      0x44
#define AUX_MU_IIR      0x48
#define AUX_MU_LCR      0x4c
#define AUX_MU_MCR      0x50
#define AUX_MU_LSR      0x54
#define AUX_MU_CNTL     0x60
#define AUX_MU_BAUD     0x68

#define ENABLES_MU      (1u << 0)
#define LCR_8BIT        3u
#define IIR_CLEAR_FIFOS 0xc6u
#define LSR_DATA_READY  (1u << 0)
#define LSR_TX_EMPTY    (1u << 5)
#define CNTL_RX_TX      3u

static uint32_t mu_base;

int uart_mini_init(uint32_t baud)
{
    const robco_board_t *b = board_get();
    uint32_t aux;
    uint32_t core;
    uint32_t reg;

    if (b->periph_base == 0) {
        return UART_MINI_ERR_BOARD;
    }
    if (baud == 0) {
        return UART_MINI_ERR_BAUD;
    }
    if (mbox_get_clock_rate(MBOX_CLOCK_CORE, &core) != 0 || core == 0) {
        core = UART_MINI_FALLBACK_CORE_CLOCK;
    }
    if (baud > core / 8u) {
        return UART_MINI_ERR_BAUD;
    }
    /* baud = core / (8 * (reg + 1)), so reg = core / (8 * baud) - 1, rounded. */
    reg = (core + 4u * baud) / (8u * baud);
    if (reg == 0 || reg - 1 > 0xffffu) {
        return UART_MINI_ERR_BAUD;
    }
    reg -= 1;

    aux = b->periph_base + AUX_OFFSET;
    mmio_write(aux + AUX_ENABLES, mmio_read(aux + AUX_ENABLES) | ENABLES_MU);
    mmio_write(aux + AUX_MU_CNTL, 0);
    mmio_write(aux + AUX_MU_IER, 0);
    mmio_write(aux + AUX_MU_LCR, LCR_8BIT);
    mmio_write(aux + AUX_MU_MCR, 0);
    mmio_write(aux + AUX_MU_IIR, IIR_CLEAR_FIFOS);
    mmio_write(aux + AUX_MU_BAUD, reg);
    mmio_write(aux + AUX_MU_CNTL, CNTL_RX_TX);
    mu_base = aux;
    return 0;
}

static void mu_put_raw(char c)
{
    while (!(mmio_read(mu_base + AUX_MU_LSR) & LSR_TX_EMPTY)) {
    }
    mmio_write(mu_base + AUX_MU_IO, (uint32_t)(unsigned char)c);
}

void uart_mini_putc(char c)
{
    if (mu_base == 0) {
        return;
    }
    if (c == '\n') {
        mu_put_raw('\r');
    }
    mu_put_raw(c);
}

int uart_mini_poll(void)
{
    if (mu_base == 0 || !(mmio_read(mu_base + AUX_MU_LSR) & LSR_DATA_READY)) {
        return -1;
    }
    return (int)(mmio_read(mu_base + AUX_MU_IO) & 0xff);
}

int uart_mini_getc(void)
{
    int c;

    while ((c = uart_mini_poll()) < 0) {
    }
    return c;
}

void uart_mini_puts(const char *s)
{
    while (*s) {
        uart_mini_putc(*s++);
    }
}
