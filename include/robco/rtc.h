#ifndef ROBCO_RTC_H
#define ROBCO_RTC_H

#include <robco/datetime.h>

/*
 * DS1307 real-time clock at I2C address 0x68, through i2c.h. The DS1338
 * and DS1340 use the same time registers. The DS1337 and DS3231 share
 * registers 0 to 6 but have no clock-halt bit and use bit 7 of the month
 * as a century flag, which rtc_get() rejects. The chip keeps UTC by
 * convention.
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

#define CLOCK_ERR_ARG       (-1)    /* NULL */
#define CLOCK_ERR_UNSET     (-2)    /* clock_set() never ran */

/*
 * Probes 0x68. Needs i2c_init(). Returns 0 when present, RTC_ERR_ABSENT
 * when nothing answers, or an I2C error. When the chip runs and holds a
 * valid time, also calls clock_set() with it.
 */
int rtc_init(void);

/* 1 after rtc_init() found the chip, else 0. */
int rtc_present(void);

/*
 * Reads all seven time registers in one transfer. Returns 0,
 * RTC_ERR_HALTED, RTC_ERR_DATA, RTC_ERR_ABSENT or an I2C error.
 * The weekday comes from the date, not from the chip.
 */
int rtc_get(datetime_t *out);

/* Writes the time in 24-hour mode and starts the oscillator. */
int rtc_set(const datetime_t *dt);

/* 1 if the oscillator runs, 0 if halted, or a negative error. */
int rtc_running(void);

/* Sets the software clock. An invalid date leaves it unchanged. */
void clock_set(const datetime_t *dt);

/* Current software time. Returns 0, CLOCK_ERR_UNSET or CLOCK_ERR_ARG. */
int clock_get(datetime_t *out);

#endif
