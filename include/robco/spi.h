#ifndef ROBCO_SPI_H
#define ROBCO_SPI_H

#include <stddef.h>
#include <stdint.h>

/*
 * SPI0 master, polled, main context only. Pins at ALT0: GPIO 8 (CE0),
 * 7 (CE1), 9 (MISO), 10 (MOSI), 11 (SCLK). 8-bit words, MSB first.
 * Every wait fails after 20 ms plus four byte times with no byte moved.
 */

#define SPI_CS_COUNT        2u      /* CE0 and CE1 */

/* `mode` for spi_configure(): an SPI mode, optionally ORed with SPI_CS_HIGH. */
#define SPI_MODE_0          0u      /* CPOL 0, CPHA 0 */
#define SPI_MODE_1          1u      /* CPOL 0, CPHA 1 */
#define SPI_MODE_2          2u      /* CPOL 1, CPHA 0 */
#define SPI_MODE_3          3u      /* CPOL 1, CPHA 1 */
#define SPI_CS_HIGH         4u      /* chip select active high */

#define SPI_DEFAULT_HZ      1000000u

/* Core clock used when the mailbox gives no answer. */
#define SPI_CORE_FALLBACK_HZ 250000000u

#define SPI_ERR_ARG         (-1)    /* bad chip select, rate, mode or buffer */
#define SPI_ERR_INIT        (-2)    /* spi_init() has not run or failed */
#define SPI_ERR_TIMEOUT     (-3)    /* the controller did not finish in time */
#define SPI_ERR_BUSY        (-4)    /* another chip select is between begin and end */

/*
 * Muxes the five pins to ALT0, reads the core clock and sets both chip
 * selects to SPI_DEFAULT_HZ, mode 0, active low. Returns 0 or SPI_ERR_INIT.
 */
int spi_init(void);

/*
 * Sets the rate and mode used for `cs`. The divider rounds up to an even
 * value, so the bus never runs faster than `hz`. Takes effect at the next
 * spi_begin() or transfer. Returns 0, SPI_ERR_ARG, SPI_ERR_INIT or
 * SPI_ERR_BUSY while a transaction is open.
 */
int spi_configure(unsigned cs, uint32_t hz, unsigned mode);

/* Rate `cs` runs at after rounding, or 0 before spi_init(). */
uint32_t spi_get_hz(unsigned cs);

/*
 * Clocks `len` bytes. `tx` NULL sends zeros, `rx` NULL drops what comes
 * in. Chip select stays asserted for the whole call. Inside a
 * spi_begin()/spi_end() pair it stays asserted after the call.
 * Returns 0 or a negative SPI_ERR_*. A timeout ends any open transaction.
 */
int spi_transfer(unsigned cs, const void *tx, void *rx, size_t len);

/* spi_transfer() with `rx` NULL. */
int spi_write(unsigned cs, const void *tx, size_t len);

/*
 * Asserts `cs` until spi_end(), so several transfers share one
 * transaction. The caller may toggle other GPIOs in between.
 */
int spi_begin(unsigned cs);

/* Waits for the last byte to go out, then releases `cs`. */
int spi_end(unsigned cs);

#endif
