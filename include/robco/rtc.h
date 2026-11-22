#ifndef ROBCO_RTC_H
#define ROBCO_RTC_H

#include <stdint.h>

#include <robco/datetime.h>

/*
 * Real-time clock at I2C address 0x68, through i2c.h: a DS1307, DS1338
 * or DS3231. rtc_init() tells the two families apart, see rtc_chip(). The
 * chip keeps UTC by convention.
 *
 * The software clock (clock_*) counts on the system timer from the last
 * clock_set(). rtc_init() seeds it from the chip when one runs.
 */

#define RTC_I2C_ADDR        0x68u

#define RTC_ERR_ARG         (-10)   /* NULL or invalid date and time */
#define RTC_ERR_ABSENT      (-11)   /* no device acknowledges 0x68 */
#define RTC_ERR_HALTED      (-12)   /* oscillator stopped: the time is not valid */
#define RTC_ERR_DATA        (-13)   /* registers hold a value that is not a date */
/* I2C errors pass through as I2C_ERR_* codes. */

#define RTC_CHIP_AUTO       (-2)    /* rtc_force_chip(), ROBCO_RTC_CHIP: detect */
#define RTC_CHIP_NONE       (-1)    /* rtc_init() found no chip */
#define RTC_CHIP_DS1307     0       /* DS1307 or DS1338: clock-halt bit, RAM at 0x08 */
#define RTC_CHIP_DS3231     1       /* DS3231: century bit, status register at 0x0F */

#define RTC_TIME_REGS       7u      /* registers 0x00 to 0x06 */
#define RTC_DS3231_STATUS   0x0fu
#define RTC_DS3231_OSF      0x80u   /* oscillator stopped since the flag was cleared */
#define RTC_PROBE_LEN       0x1au   /* bytes rtc_detect() reads from register 0x00 */

#define CLOCK_ERR_ARG       (-1)    /* NULL */
#define CLOCK_ERR_UNSET     (-2)    /* clock_set() never ran */

/*
 * Probes 0x68. Needs i2c_init(). Returns 0 when present, RTC_ERR_ABSENT
 * when nothing answers or the chip reads as all 0x00 or all 0xFF, or an
 * I2C error. Detects the chip with reads only, see rtc_detect(), unless
 * rtc_force_chip() or ROBCO_RTC_CHIP names it. Never writes the chip.
 * When the chip runs and holds a valid time, also calls clock_set().
 */
int rtc_init(void);

/*
 * Sets the chip class for the next rtc_init(): RTC_CHIP_AUTO,
 * RTC_CHIP_DS1307 or RTC_CHIP_DS3231. Overrides ROBCO_RTC_CHIP.
 * Returns 0 or RTC_ERR_ARG.
 */
int rtc_force_chip(int chip);

/* 1 after rtc_init() found the chip, else 0. */
int rtc_present(void);

/* RTC_CHIP_* that rtc_init() detected, or RTC_CHIP_NONE. */
int rtc_chip(void);

/*
 * Reads all seven time registers in one transfer. Never writes. Returns 0,
 * RTC_ERR_HALTED, RTC_ERR_DATA, RTC_ERR_ABSENT or an I2C error.
 * The weekday comes from the date, not from the chip.
 */
int rtc_get(datetime_t *out);

/*
 * The only call that writes the chip. Writes registers 0x00 to 0x06 in
 * 24-hour mode with clock halt clear, which starts the oscillator. On a
 * DS3231 it then reads status (0x0F) and writes it back with only the
 * oscillator-stop flag cleared.
 */
int rtc_set(const datetime_t *dt);

/* 1 if the oscillator runs, 0 if halted, or a negative error. */
int rtc_running(void);

/* Sets the software clock. An invalid date leaves it unchanged. */
void clock_set(const datetime_t *dt);

/* Current software time. Returns 0, CLOCK_ERR_UNSET or CLOCK_ERR_ARG. */
int clock_get(datetime_t *out);

/* ---- Pure register codec, src/lib/rtc_regs.c ---- */

/*
 * Decodes registers 0 to 6 of `chip`. `status` is the DS3231 status
 * register and is ignored for a DS1307. Returns 0, RTC_ERR_HALTED,
 * RTC_ERR_DATA or RTC_ERR_ARG.
 */
int rtc_regs_decode(int chip, const uint8_t regs[RTC_TIME_REGS], uint8_t status,
                    datetime_t *out);

/* Encodes `dt` in 24-hour mode with the halt and century bits clear. 0 or RTC_ERR_ARG. */
int rtc_regs_encode(const datetime_t *dt, uint8_t regs[RTC_TIME_REGS]);

/*
 * Classifies a chip from RTC_PROBE_LEN bytes read in one transfer from
 * register 0x00. The DS3231 pointer wraps from 0x12 to 0x00, so bytes
 * 0x13 to 0x19 repeat the time registers. Returns RTC_CHIP_DS3231 only
 * when all of these hold:
 *   - both copies of 0x00 to 0x06 decode as a valid DS3231 time, and the
 *     second is the same or one second later (a tick during the read);
 *   - status 0x0F bits 6 to 4 read 0;
 *   - temperature LSB 0x12 bits 5 to 0 read 0.
 * Returns RTC_CHIP_NONE when all bytes are 0x00 or all are 0xFF: a dead
 * device or a stuck bus. Otherwise RTC_CHIP_DS1307: a DS1307 or DS1338,
 * or a DS3231 with an invalid time, which rtc_set() then repairs.
 * Returns RTC_CHIP_NONE for NULL.
 */
int rtc_detect(const uint8_t regs[RTC_PROBE_LEN]);

#endif
