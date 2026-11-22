#ifndef ROBCO_DATETIME_H
#define ROBCO_DATETIME_H

#include <stddef.h>
#include <stdint.h>

/*
 * Calendar date and time, 2000-01-01 00:00:00 to 2099-12-31 23:59:59.
 * Pure functions, src/lib/datetime.c. No time zones: callers keep UTC.
 */

typedef struct {
    uint16_t year;      /* 2000 to 2099 */
    uint8_t month;      /* 1 to 12 */
    uint8_t day;        /* 1 to 28, 29, 30 or 31 */
    uint8_t hour;       /* 0 to 23 */
    uint8_t minute;     /* 0 to 59 */
    uint8_t second;     /* 0 to 59 */
    uint8_t weekday;    /* 0 Sunday to 6 Saturday */
} datetime_t;

#define DT_YEAR_MIN     2000u
#define DT_YEAR_MAX     2099u

/* Seconds from 2000-01-01 00:00:00 to 2099-12-31 23:59:59. */
#define DT_SECONDS_MAX  3155759999u

/* "YYYY-MM-DDTHH:MM:SS" plus the NUL. */
#define DT_ISO_SIZE     20u

#define DT_ERR_ARG      (-1)    /* NULL pointer or buffer too small */
#define DT_ERR_RANGE    (-2)    /* field or value out of range */

/* 1 for a leap year, else 0. Gregorian rule, any year. */
int dt_is_leap(unsigned year);

/* Days in `month` of `year`, or 0 for a month outside 1 to 12. */
unsigned dt_days_in_month(unsigned year, unsigned month);

/* Day of week, 0 Sunday to 6 Saturday, or -1 for an invalid date in range. */
int dt_weekday(unsigned year, unsigned month, unsigned day);

/*
 * 0 if every field except `weekday` is in range, else DT_ERR_RANGE.
 * DT_ERR_ARG for NULL. The weekday is not checked: the other fields fix it.
 */
int dt_valid(const datetime_t *dt);

/* Seconds since 2000-01-01 00:00:00. Returns 0 or a DT_ERR_* code. */
int dt_to_seconds(const datetime_t *dt, uint32_t *out);

/* The inverse, with the weekday filled in. DT_ERR_RANGE above DT_SECONDS_MAX. */
int dt_from_seconds(uint32_t seconds, datetime_t *out);

/*
 * FAT directory entry stamps. Date: bits 15:9 year - 1980, 8:5 month,
 * 4:0 day. Time: bits 15:11 hour, 10:5 minute, 4:0 second / 2.
 * Odd seconds round down.
 */
int dt_to_fat(const datetime_t *dt, uint16_t *fat_date, uint16_t *fat_time);

/* The inverse. Rejects stamps outside 2000 to 2099 and invalid fields. */
int dt_from_fat(uint16_t fat_date, uint16_t fat_time, datetime_t *out);

/* Packs 0 to 99 into two BCD digits. Returns 0xff above 99. */
uint8_t dt_bcd_encode(unsigned value);

/* Unpacks two BCD digits, or returns -1 when a digit is above 9. */
int dt_bcd_decode(uint8_t bcd);

/*
 * Writes "YYYY-MM-DDTHH:MM:SS" and a NUL. `size` must be at least
 * DT_ISO_SIZE. Returns 19 or a DT_ERR_* code; on error `buf` holds ""
 * when `size` is not 0.
 */
int dt_format_iso(const datetime_t *dt, char *buf, size_t size);

#endif
