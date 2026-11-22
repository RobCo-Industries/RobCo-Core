#ifndef ROBCO_I2C_H
#define ROBCO_I2C_H

#include <stddef.h>
#include <stdint.h>

/*
 * I2C master on the BSC1 controller, GPIO 2 (SDA) and 3 (SCL), 7-bit
 * addresses. Polled, main context only. Every transfer has a deadline on
 * the system timer.
 */

#define I2C_HZ_MIN          10000u
#define I2C_HZ_MAX          1000000u
#define I2C_ADDR_MAX        0x7fu

/* Core clock used when the mailbox gives no answer. */
#define I2C_CORE_FALLBACK_HZ 250000000u

#define I2C_ERR_ARG         (-1)    /* bad address, length, buffer or rate */
#define I2C_ERR_INIT        (-2)    /* i2c_init() has not run or failed */
#define I2C_ERR_NACK        (-3)    /* the address or a data byte was not acknowledged */
#define I2C_ERR_TIMEOUT     (-4)    /* the transfer did not finish in time */
#define I2C_ERR_CLKT        (-5)    /* the slave held SCL low too long */

/*
 * Muxes GPIO 2 and 3 to ALT0, sets the divider for `hz` from the core
 * clock and the clock stretch timeout. Can run again to change the rate.
 * Returns 0 or I2C_ERR_ARG / I2C_ERR_INIT.
 */
int i2c_init(uint32_t hz);

/* SCL rate after the divider rounds, or 0 before i2c_init(). */
uint32_t i2c_get_hz(void);

/* START, address with W, `len` bytes, STOP. `len` must be at least 1. */
int i2c_write(uint8_t addr, const void *buf, size_t len);

/* START, address with R, `len` bytes, STOP. `len` must be at least 1. */
int i2c_read(uint8_t addr, void *buf, size_t len);

/*
 * Writes `wbuf`, then reads `rbuf` as a second transfer: STOP then START,
 * not a repeated start. Fine for register-pointer devices such as RTCs
 * and EEPROMs.
 */
int i2c_write_read(uint8_t addr, const void *wbuf, size_t wlen, void *rbuf, size_t rlen);

/* Reads one byte: 1 if `addr` acknowledges, 0 if not, or a negative error. */
int i2c_probe(uint8_t addr);

/* Short text for an I2C_ERR_* code. */
const char *i2c_strerror(int code);

#endif
