/*
 * Time register encode and decode for the DS1307 family and the DS3231.
 * Pure: the driver in src/drv/rtc_ds1307.c does the I2C transfers.
 *
 * Registers 0 to 6 hold seconds, minutes, hours, weekday, day, month and
 * year in BCD on both families. The differences:
 *   DS1307, DS1338: bit 7 of the seconds is the clock-halt bit.
 *   DS3231: bit 7 of the seconds is always 0, bit 7 of the month is the
 *   century bit, and the oscillator-stop flag is bit 7 of status (0x0F).
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/datetime.h>
#include <robco/rtc.h>

#define SEC_CH          0x80u   /* DS1307 clock halt */
#define HOUR_12         0x40u   /* 12-hour mode */
#define HOUR_PM         0x20u
#define MONTH_CENTURY   0x80u   /* DS3231 */

/* Decodes BCD after masking, and checks lo <= value <= hi. */
static int field(uint8_t raw, uint8_t mask, int lo, int hi)
{
    int v = dt_bcd_decode(raw & mask);

    if (v < lo || v > hi) {
        return -1;
    }
    return v;
}

static int decode_hour(uint8_t raw)
{
    int h;

    if (raw & 0x80u) {
        return -1;
    }
    if (!(raw & HOUR_12)) {
        return field(raw, 0x3f, 0, 23);
    }
    h = field(raw, 0x1f, 1, 12);
    if (h < 0) {
        return -1;
    }
    if (h == 12) {
        h = 0;
    }
    return (raw & HOUR_PM) ? h + 12 : h;
}

int rtc_regs_decode(int chip, const uint8_t regs[RTC_TIME_REGS], uint8_t status,
                    datetime_t *out)
{
    int sec, min, hour, day, mon, year;
    uint8_t mon_reserved;

    if (regs == NULL || out == NULL ||
        (chip != RTC_CHIP_DS1307 && chip != RTC_CHIP_DS3231)) {
        return RTC_ERR_ARG;
    }
    if (chip == RTC_CHIP_DS1307) {
        if (regs[0] & SEC_CH) {
            return RTC_ERR_HALTED;
        }
        mon_reserved = 0xe0u;
    } else {
        if (status & RTC_DS3231_OSF) {
            return RTC_ERR_HALTED;
        }
        if (regs[0] & 0x80u) {
            return RTC_ERR_DATA;
        }
        /* The century bit is masked: years are 2000 to 2099. */
        mon_reserved = 0x60u;
    }
    sec = field(regs[0], 0x7f, 0, 59);
    min = field(regs[1], 0x7f, 0, 59);
    hour = decode_hour(regs[2]);
    day = field(regs[4], 0x3f, 1, 31);
    mon = field(regs[5], 0x1f, 1, 12);
    year = field(regs[6], 0xff, 0, 99);
    if (sec < 0 || min < 0 || hour < 0 || day < 0 || mon < 0 || year < 0 ||
        (regs[1] & 0x80u) || (regs[4] & 0xc0u) || (regs[5] & mon_reserved)) {
        return RTC_ERR_DATA;
    }
    if (day > (int)dt_days_in_month(2000u + (unsigned)year, (unsigned)mon)) {
        return RTC_ERR_DATA;
    }
    out->year = (uint16_t)(2000 + year);
    out->month = (uint8_t)mon;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)hour;
    out->minute = (uint8_t)min;
    out->second = (uint8_t)sec;
    out->weekday = (uint8_t)dt_weekday(out->year, out->month, out->day);
    return 0;
}

int rtc_regs_encode(const datetime_t *dt, uint8_t regs[RTC_TIME_REGS])
{
    if (dt == NULL || regs == NULL || dt_valid(dt) != 0) {
        return RTC_ERR_ARG;
    }
    regs[0] = dt_bcd_encode(dt->second);     /* clock halt 0 starts a DS1307 */
    regs[1] = dt_bcd_encode(dt->minute);
    regs[2] = dt_bcd_encode(dt->hour);       /* bit 6 0: 24-hour mode */
    regs[3] = (uint8_t)(dt_weekday(dt->year, dt->month, dt->day) + 1); /* 1 Sunday */
    regs[4] = dt_bcd_encode(dt->day);
    regs[5] = dt_bcd_encode(dt->month);      /* century bit 0 on a DS3231 */
    regs[6] = dt_bcd_encode(dt->year - 2000u);
    return 0;
}
