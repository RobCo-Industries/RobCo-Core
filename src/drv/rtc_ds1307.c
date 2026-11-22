#include <stdint.h>

#include <robco/datetime.h>
#include <robco/i2c.h>
#include <robco/rtc.h>
#include <robco/timer.h>

/*
 * Driver for the DS1307 family (DS1307, DS1338) and the DS3231 at 0x68.
 * The register codec is pure, in src/lib/rtc_regs.c.
 *
 * Detection reads only: rtc_init() never writes the chip. Another device
 * may answer at 0x68, and the DS1307 RAM belongs to the user. rtc_detect()
 * in src/lib/rtc_regs.c applies the rule. rtc_force_chip() or
 * ROBCO_RTC_CHIP skips it. Only rtc_set() writes.
 */
#ifndef ROBCO_RTC_CHIP
#define ROBCO_RTC_CHIP  RTC_CHIP_AUTO
#endif
#if ROBCO_RTC_CHIP != RTC_CHIP_AUTO && ROBCO_RTC_CHIP != RTC_CHIP_DS1307 && \
    ROBCO_RTC_CHIP != RTC_CHIP_DS3231
#error "ROBCO_RTC_CHIP must be RTC_CHIP_AUTO, RTC_CHIP_DS1307 or RTC_CHIP_DS3231"
#endif

#define REG_SECONDS     0x00u
#define SEC_CH          0x80u   /* DS1307 clock halt */

static int forced = ROBCO_RTC_CHIP;
static int present;
static int chip = RTC_CHIP_NONE;

static int clock_is_set;
static uint32_t clock_base_s;   /* seconds since 2000 at clock_base_us */
static uint64_t clock_base_us;

static int read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_write_read(RTC_I2C_ADDR, &reg, 1, val, 1);
}

static int write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    return i2c_write(RTC_I2C_ADDR, buf, sizeof(buf));
}

static int detect(void)
{
    uint8_t ptr = REG_SECONDS;
    uint8_t r[RTC_PROBE_LEN];
    int rc = i2c_write_read(RTC_I2C_ADDR, &ptr, 1, r, sizeof(r));

    if (rc < 0) {
        return rc;
    }
    if (forced != RTC_CHIP_AUTO) {
        /* Still read: an all 0x00 or 0xFF answer means no working chip. */
        return rtc_detect(r) == RTC_CHIP_NONE ? RTC_CHIP_NONE : forced;
    }
    return rtc_detect(r);
}

int rtc_force_chip(int which)
{
    if (which != RTC_CHIP_AUTO && which != RTC_CHIP_DS1307 &&
        which != RTC_CHIP_DS3231) {
        return RTC_ERR_ARG;
    }
    forced = which;
    return 0;
}

int rtc_init(void)
{
    datetime_t now;
    int rc;

    present = 0;
    chip = RTC_CHIP_NONE;
    rc = i2c_probe(RTC_I2C_ADDR);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return RTC_ERR_ABSENT;
    }
    rc = detect();
    if (rc < 0) {
        return rc;
    }
    if (rc == RTC_CHIP_NONE) {
        return RTC_ERR_ABSENT;
    }
    chip = rc;
    present = 1;
    if (rtc_get(&now) == 0) {
        clock_set(&now);
    }
    return 0;
}

int rtc_present(void)
{
    return present;
}

int rtc_chip(void)
{
    return chip;
}

int rtc_get(datetime_t *out)
{
    uint8_t ptr = REG_SECONDS;
    uint8_t r[RTC_TIME_REGS];
    uint8_t status = 0;
    int rc;

    if (out == NULL) {
        return RTC_ERR_ARG;
    }
    if (!present) {
        return RTC_ERR_ABSENT;
    }
    rc = i2c_write_read(RTC_I2C_ADDR, &ptr, 1, r, sizeof(r));
    if (rc >= 0 && chip == RTC_CHIP_DS3231) {
        rc = read_reg(RTC_DS3231_STATUS, &status);
    }
    if (rc < 0) {
        return rc;
    }
    return rtc_regs_decode(chip, r, status, out);
}

int rtc_set(const datetime_t *dt)
{
    uint8_t buf[1 + RTC_TIME_REGS];
    uint8_t status;
    int rc;

    if (rtc_regs_encode(dt, buf + 1) != 0) {
        return RTC_ERR_ARG;
    }
    if (!present) {
        return RTC_ERR_ABSENT;
    }
    buf[0] = REG_SECONDS;
    rc = i2c_write(RTC_I2C_ADDR, buf, sizeof(buf));
    if (rc < 0 || chip != RTC_CHIP_DS3231) {
        return rc;
    }
    /*
     * The time is valid again: clear the oscillator-stop flag. Read-modify-
     * write keeps EN32kHz and the alarm flags. An alarm flag that rises
     * between the read and the write is cleared, as with any status write.
     */
    rc = read_reg(RTC_DS3231_STATUS, &status);
    if (rc < 0) {
        return rc;
    }
    return write_reg(RTC_DS3231_STATUS, (uint8_t)(status & ~RTC_DS3231_OSF));
}

int rtc_running(void)
{
    uint8_t v;
    int rc;

    if (!present) {
        return RTC_ERR_ABSENT;
    }
    if (chip == RTC_CHIP_DS3231) {
        rc = read_reg(RTC_DS3231_STATUS, &v);
        return rc < 0 ? rc : (v & RTC_DS3231_OSF) ? 0 : 1;
    }
    rc = read_reg(REG_SECONDS, &v);
    return rc < 0 ? rc : (v & SEC_CH) ? 0 : 1;
}

void clock_set(const datetime_t *dt)
{
    uint32_t s;

    if (dt == NULL || dt_to_seconds(dt, &s) != 0) {
        return;
    }
    clock_base_s = s;
    clock_base_us = timer_now_us();
    clock_is_set = 1;
}

int clock_get(datetime_t *out)
{
    uint64_t s;

    if (out == NULL) {
        return CLOCK_ERR_ARG;
    }
    if (!clock_is_set) {
        return CLOCK_ERR_UNSET;
    }
    s = clock_base_s + (timer_now_us() - clock_base_us) / 1000000u;
    if (s > DT_SECONDS_MAX) {
        s = DT_SECONDS_MAX;
    }
    return dt_from_seconds((uint32_t)s, out);
}
