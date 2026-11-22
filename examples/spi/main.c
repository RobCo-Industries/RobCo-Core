#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/klibc.h>
#include <robco/mmio.h>
#include <robco/spi.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define SPI0_CS     0x204000u
#define LONG_LEN    300u    /* well above the FIFO depth */
#define DC_PIN      25u     /* stand-in data/command pin */

static uint8_t tx[LONG_LEN];
static uint8_t rx[LONG_LEN];

/* NULL the compiler cannot see, for argument checks. */
static void *null_ptr(void)
{
    void *volatile p = 0;

    return p;
}

static uint32_t cs_reg(void)
{
    return mmio_read(board_get()->periph_base + SPI0_CS);
}

/* Counts bytes of rx[0..n) equal to `v`. */
static unsigned count(size_t n, uint8_t v)
{
    unsigned c = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        c += rx[i] == v;
    }
    return c;
}

static void args(void)
{
    k_printf("@RC stage=spi ev=args cs=%d mode=%d hz=%d begin=%d end=%d xfer=%d write=%d "
             "empty=%d\n",
             spi_configure(2, 1000000, SPI_MODE_0),
             spi_configure(0, 1000000, 8),
             spi_configure(0, 0, SPI_MODE_0),
             spi_begin(2), spi_end(2),
             spi_transfer(2, tx, rx, 1),
             spi_write(0, null_ptr(), 4),
             spi_transfer(0, tx, rx, 0));
}

void robco_main(void)
{
    unsigned cs;
    unsigned mode;
    uint32_t reg;
    uint64_t t0;
    int rc;
    int rc2;
    int rc3;
    size_t i;

    uart_init(115200);
    k_printf("\n@RC stage=spi ev=boot\n");

    k_printf("@RC stage=spi ev=preinit xfer=%d\n", spi_transfer(0, tx, rx, 1));
    rc = spi_init();
    k_printf("@RC stage=spi ev=init rc=%d hz0=%u hz1=%u fsel=%d%d%d%d%d\n", rc,
             (unsigned)spi_get_hz(0), (unsigned)spi_get_hz(1),
             gpio_get_function(7), gpio_get_function(8), gpio_get_function(9),
             gpio_get_function(10), gpio_get_function(11));
    /*
     * JEDEC ID first: an attached QEMU SPI flash never sees chip select
     * change, so only the first command on the bus reaches it.
     */
    k_memset(rx, 0x55, 4);
    tx[0] = 0x9f;
    tx[1] = tx[2] = tx[3] = 0;
    rc = spi_transfer(0, tx, rx, 4);
    k_printf("@RC stage=spi ev=jedec rc=%d id=%02x%02x%02x\n", rc, rx[1], rx[2], rx[3]);

    for (i = 0; i < LONG_LEN; i++) {
        tx[i] = (uint8_t)(i * 13u + 1u);
    }
    args();

    /* Every mode on both chip selects; check what the controller latched. */
    for (cs = 0; cs < SPI_CS_COUNT; cs++) {
        for (mode = 0; mode < 4; mode++) {
            rc = spi_configure(cs, 500000, mode);
            rc2 = spi_begin(cs);
            reg = cs_reg();
            k_memset(rx, 0x55, 8);
            rc3 = spi_transfer(cs, tx, rx, 8);
            k_printf("@RC stage=spi ev=mode cs=%u mode=%u cfg=%d begin=%d xfer=%d end=%d "
                     "cs_bits=%u cpha=%u cpol=%u ta=%u zeros=%u\n",
                     cs, mode, rc, rc2, rc3, spi_end(cs),
                     (unsigned)(reg & 3u), (unsigned)((reg >> 2) & 1u),
                     (unsigned)((reg >> 3) & 1u), (unsigned)((reg >> 7) & 1u), count(8, 0));
        }
    }
    reg = cs_reg();
    k_printf("@RC stage=spi ev=idle ta=%u\n", (unsigned)((reg >> 7) & 1u));

    /* Chip select polarity. */
    rc = spi_configure(1, 1000000, SPI_MODE_0 | SPI_CS_HIGH);
    spi_begin(1);
    reg = cs_reg();
    spi_end(1);
    k_printf("@RC stage=spi ev=cspol cfg=%d cspol=%u cspol1=%u cspol0=%u\n", rc,
             (unsigned)((reg >> 6) & 1u), (unsigned)((reg >> 22) & 1u),
             (unsigned)((reg >> 21) & 1u));
    spi_configure(1, 1000000, SPI_MODE_0);

    /* Long transfers: full duplex, write only, read only. */
    k_memset(rx, 0x55, LONG_LEN);
    t0 = timer_now_us();
    rc = spi_transfer(0, tx, rx, LONG_LEN);
    k_printf("@RC stage=spi ev=long len=%u rc=%d zeros=%u us=%u\n", LONG_LEN, rc,
             count(LONG_LEN, 0), (unsigned)(timer_now_us() - t0));
    k_memset(rx, 0x55, LONG_LEN);
    rc = spi_write(0, tx, LONG_LEN);
    rc2 = spi_transfer(0, null_ptr(), rx, 100);
    k_printf("@RC stage=spi ev=halves write=%d read=%d zeros=%u untouched=%u\n",
             rc, rc2, count(100, 0), count(LONG_LEN, 0x55));

    /* One transaction, a GPIO toggled between two writes, as a display driver does. */
    gpio_config_output(DC_PIN, 0);
    rc = spi_begin(0);
    rc2 = spi_write(0, tx, 1);
    gpio_write(DC_PIN, 1);
    rc3 = spi_write(0, tx + 1, 32);
    reg = cs_reg();
    k_printf("@RC stage=spi ev=txn begin=%d cmd=%d data=%d ta=%u other=%d other_begin=%d "
             "other_cfg=%d end=%d\n",
             rc, rc2, rc3, (unsigned)((reg >> 7) & 1u),
             spi_transfer(1, tx, rx, 1), spi_begin(1),
             spi_configure(1, 1000000, SPI_MODE_0), spi_end(0));

    k_printf("@RC stage=spi ev=done\n");
    for (;;) {
        timer_delay_ms(1000);
    }
}
