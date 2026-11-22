#include <stddef.h>
#include <stdint.h>

#include <robco/datetime.h>

#define SECONDS_PER_DAY 86400u
#define FAT_YEAR_BASE   1980u

/* Days before the first of each month in a common year. */
static const uint16_t days_before[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
};

int dt_is_leap(unsigned year)
{
    if (year % 400 == 0) {
        return 1;
    }
    if (year % 100 == 0) {
        return 0;
    }
    return year % 4 == 0;
}

unsigned dt_days_in_month(unsigned year, unsigned month)
{
    static const uint8_t len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && dt_is_leap(year)) {
        return 29;
    }
    return len[month - 1];
}

static int date_ok(unsigned year, unsigned month, unsigned day)
{
    return year >= DT_YEAR_MIN && year <= DT_YEAR_MAX
        && month >= 1 && month <= 12
        && day >= 1 && day <= dt_days_in_month(year, month);
}

/* Days from 2000-01-01 to the date. The date must be valid. */
static uint32_t days_since_2000(unsigned year, unsigned month, unsigned day)
{
    unsigned y = year - DT_YEAR_MIN;
    /* Leap years in [2000, year): 2000 is one, and no century rule applies before 2100. */
    uint32_t days = y * 365u + (y + 3u) / 4u;

    days += days_before[month - 1];
    if (month > 2 && dt_is_leap(year)) {
        days++;
    }
    return days + day - 1u;
}

int dt_weekday(unsigned year, unsigned month, unsigned day)
{
    if (!date_ok(year, month, day)) {
        return -1;
    }
    /* 2000-01-01 was a Saturday. */
    return (int)((days_since_2000(year, month, day) + 6u) % 7u);
}

int dt_valid(const datetime_t *dt)
{
    if (dt == NULL) {
        return DT_ERR_ARG;
    }
    if (!date_ok(dt->year, dt->month, dt->day)
        || dt->hour > 23 || dt->minute > 59 || dt->second > 59) {
        return DT_ERR_RANGE;
    }
    return 0;
}

int dt_to_seconds(const datetime_t *dt, uint32_t *out)
{
    int rc;

    if (out == NULL) {
        return DT_ERR_ARG;
    }
    rc = dt_valid(dt);
    if (rc < 0) {
        return rc;
    }
    *out = days_since_2000(dt->year, dt->month, dt->day) * SECONDS_PER_DAY
         + dt->hour * 3600u + dt->minute * 60u + dt->second;
    return 0;
}

int dt_from_seconds(uint32_t seconds, datetime_t *out)
{
    uint32_t days;
    uint32_t rem;
    unsigned year = DT_YEAR_MIN;
    unsigned month = 1;
    unsigned ylen;
    unsigned mlen;

    if (out == NULL) {
        return DT_ERR_ARG;
    }
    if (seconds > DT_SECONDS_MAX) {
        return DT_ERR_RANGE;
    }
    days = seconds / SECONDS_PER_DAY;
    rem = seconds % SECONDS_PER_DAY;
    out->weekday = (uint8_t)((days + 6u) % 7u);
    out->hour = (uint8_t)(rem / 3600u);
    out->minute = (uint8_t)(rem / 60u % 60u);
    out->second = (uint8_t)(rem % 60u);
    for (;;) {
        ylen = dt_is_leap(year) ? 366u : 365u;
        if (days < ylen) {
            break;
        }
        days -= ylen;
        year++;
    }
    for (;;) {
        mlen = dt_days_in_month(year, month);
        if (days < mlen) {
            break;
        }
        days -= mlen;
        month++;
    }
    out->year = (uint16_t)year;
    out->month = (uint8_t)month;
    out->day = (uint8_t)(days + 1u);
    return 0;
}

int dt_to_fat(const datetime_t *dt, uint16_t *fat_date, uint16_t *fat_time)
{
    int rc;

    if (fat_date == NULL || fat_time == NULL) {
        return DT_ERR_ARG;
    }
    rc = dt_valid(dt);
    if (rc < 0) {
        return rc;
    }
    *fat_date = (uint16_t)(((dt->year - FAT_YEAR_BASE) << 9) | ((unsigned)dt->month << 5) | dt->day);
    *fat_time = (uint16_t)(((unsigned)dt->hour << 11) | ((unsigned)dt->minute << 5) | (dt->second / 2u));
    return 0;
}

int dt_from_fat(uint16_t fat_date, uint16_t fat_time, datetime_t *out)
{
    datetime_t d;

    if (out == NULL) {
        return DT_ERR_ARG;
    }
    d.year = (uint16_t)(FAT_YEAR_BASE + (fat_date >> 9));
    d.month = (uint8_t)((fat_date >> 5) & 0x0fu);
    d.day = (uint8_t)(fat_date & 0x1fu);
    d.hour = (uint8_t)(fat_time >> 11);
    d.minute = (uint8_t)((fat_time >> 5) & 0x3fu);
    d.second = (uint8_t)((fat_time & 0x1fu) * 2u);
    d.weekday = 0;
    if (dt_valid(&d) < 0) {
        return DT_ERR_RANGE;
    }
    d.weekday = (uint8_t)dt_weekday(d.year, d.month, d.day);
    *out = d;
    return 0;
}

uint8_t dt_bcd_encode(unsigned value)
{
    if (value > 99) {
        return 0xff;
    }
    return (uint8_t)(((value / 10u) << 4) | (value % 10u));
}

int dt_bcd_decode(uint8_t bcd)
{
    unsigned hi = bcd >> 4;
    unsigned lo = bcd & 0x0fu;

    if (hi > 9 || lo > 9) {
        return -1;
    }
    return (int)(hi * 10u + lo);
}

static char *put2(char *p, unsigned v)
{
    p[0] = (char)('0' + v / 10u);
    p[1] = (char)('0' + v % 10u);
    return p + 2;
}

int dt_format_iso(const datetime_t *dt, char *buf, size_t size)
{
    char *p;

    if (buf == NULL) {
        return DT_ERR_ARG;
    }
    if (size > 0) {
        buf[0] = '\0';
    }
    if (size < DT_ISO_SIZE) {
        return DT_ERR_ARG;
    }
    if (dt_valid(dt) < 0) {
        return dt == NULL ? DT_ERR_ARG : DT_ERR_RANGE;
    }
    p = put2(buf, dt->year / 100u);
    p = put2(p, dt->year % 100u);
    *p++ = '-';
    p = put2(p, dt->month);
    *p++ = '-';
    p = put2(p, dt->day);
    *p++ = 'T';
    p = put2(p, dt->hour);
    *p++ = ':';
    p = put2(p, dt->minute);
    *p++ = ':';
    p = put2(p, dt->second);
    *p = '\0';
    return (int)(p - buf);
}
