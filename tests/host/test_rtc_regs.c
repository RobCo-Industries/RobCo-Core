/* RTC register codec: DS1307 and DS1338 against DS3231 quirks. */
#include <stdint.h>

#include <robco/rtc.h>

#include "rc_test.h"

/* 2026-03-14 15:09:26, 24-hour mode, a Saturday. */
static const uint8_t base[RTC_TIME_REGS] = { 0x26, 0x09, 0x15, 0x07, 0x14, 0x03, 0x26 };

static int decode(int chip, const uint8_t *regs, uint8_t status, datetime_t *out)
{
    return rtc_regs_decode(chip, regs, status, out);
}

static void check_time(const datetime_t *dt, unsigned h, unsigned m, unsigned s)
{
    CHECK_EQ(dt->year, 2026);
    CHECK_EQ(dt->month, 3);
    CHECK_EQ(dt->day, 14);
    CHECK_EQ(dt->hour, h);
    CHECK_EQ(dt->minute, m);
    CHECK_EQ(dt->second, s);
    CHECK_EQ(dt->weekday, 6);
}

TEST(both_chips_decode_the_same_time)
{
    datetime_t dt;

    CHECK_EQ(decode(RTC_CHIP_DS1307, base, 0xff, &dt), 0);
    check_time(&dt, 15, 9, 26);
    CHECK_EQ(decode(RTC_CHIP_DS3231, base, 0x00, &dt), 0);
    check_time(&dt, 15, 9, 26);
}

TEST(ds1307_clock_halt)
{
    uint8_t r[RTC_TIME_REGS];
    datetime_t dt;

    memcpy(r, base, sizeof(r));
    r[0] |= 0x80;
    CHECK_EQ(decode(RTC_CHIP_DS1307, r, 0, &dt), RTC_ERR_HALTED);
    r[0] &= 0x7f;
    CHECK_EQ(decode(RTC_CHIP_DS1307, r, 0, &dt), 0);
    /* The DS1307 has no status register: the argument is ignored. */
    CHECK_EQ(decode(RTC_CHIP_DS1307, r, RTC_DS3231_OSF, &dt), 0);
}

TEST(ds3231_oscillator_stop_and_seconds_bit)
{
    uint8_t r[RTC_TIME_REGS];
    datetime_t dt;

    memcpy(r, base, sizeof(r));
    CHECK_EQ(decode(RTC_CHIP_DS3231, r, RTC_DS3231_OSF, &dt), RTC_ERR_HALTED);
    CHECK_EQ(decode(RTC_CHIP_DS3231, r, (uint8_t)~RTC_DS3231_OSF, &dt), 0);
    /* No clock-halt bit: bit 7 of the seconds always reads 0. */
    r[0] |= 0x80;
    CHECK_EQ(decode(RTC_CHIP_DS3231, r, 0, &dt), RTC_ERR_DATA);
}

TEST(ds3231_century_bit_masked)
{
    uint8_t r[RTC_TIME_REGS];
    datetime_t dt;

    memcpy(r, base, sizeof(r));
    r[5] |= 0x80;
    CHECK_EQ(decode(RTC_CHIP_DS3231, r, 0, &dt), 0);
    CHECK_EQ(dt.month, 3);
    CHECK_EQ(dt.year, 2026);
    /* On a DS1307 the bit is reserved. */
    CHECK_EQ(decode(RTC_CHIP_DS1307, r, 0, &dt), RTC_ERR_DATA);
    /* Bits 6 and 5 stay reserved on both. */
    r[5] = 0x43;
    CHECK_EQ(decode(RTC_CHIP_DS3231, r, 0, &dt), RTC_ERR_DATA);
}

TEST(twelve_hour_mode)
{
    static const struct { uint8_t raw; unsigned hour; } cases[] = {
        { 0x40 | 0x12, 0 },          /* 12 AM */
        { 0x40 | 0x01, 1 },          /* 1 AM */
        { 0x40 | 0x11, 11 },         /* 11 AM */
        { 0x40 | 0x20 | 0x12, 12 },  /* 12 PM */
        { 0x40 | 0x20 | 0x01, 13 },  /* 1 PM */
        { 0x40 | 0x20 | 0x11, 23 },  /* 11 PM */
    };
    uint8_t r[RTC_TIME_REGS];
    datetime_t dt;
    unsigned i;
    int chip;

    for (chip = RTC_CHIP_DS1307; chip <= RTC_CHIP_DS3231; chip++) {
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            memcpy(r, base, sizeof(r));
            r[2] = cases[i].raw;
            CHECK_EQ(decode(chip, r, 0, &dt), 0);
            CHECK_EQ(dt.hour, cases[i].hour);
        }
        memcpy(r, base, sizeof(r));
        r[2] = 0x40 | 0x00;          /* hour 0 does not exist in 12-hour mode */
        CHECK_EQ(decode(chip, r, 0, &dt), RTC_ERR_DATA);
        r[2] = 0x40 | 0x13;
        CHECK_EQ(decode(chip, r, 0, &dt), RTC_ERR_DATA);
        r[2] = 0x24;                 /* 24-hour mode past 23 */
        CHECK_EQ(decode(chip, r, 0, &dt), RTC_ERR_DATA);
    }
}

TEST(invalid_bcd_and_ranges)
{
    static const struct { unsigned reg; uint8_t val; } bad[] = {
        { 0, 0x0a }, { 0, 0x60 }, { 1, 0x5f }, { 1, 0x80 }, { 2, 0x1a }, { 2, 0x80 },
        { 4, 0x00 }, { 4, 0x32 }, { 4, 0x1c }, { 4, 0x41 }, { 5, 0x00 }, { 5, 0x13 },
        { 5, 0x0a }, { 6, 0xa0 }, { 6, 0x0f },
    };
    uint8_t r[RTC_TIME_REGS];
    datetime_t dt;
    unsigned i;
    int chip;

    for (chip = RTC_CHIP_DS1307; chip <= RTC_CHIP_DS3231; chip++) {
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            memcpy(r, base, sizeof(r));
            r[bad[i].reg] = bad[i].val;
            CHECK_EQ(decode(chip, r, 0, &dt), RTC_ERR_DATA);
        }
        /* Day past the end of the month. */
        memcpy(r, base, sizeof(r));
        r[4] = 0x31;
        r[5] = 0x04;
        CHECK_EQ(decode(chip, r, 0, &dt), RTC_ERR_DATA);
        /* The weekday register is not checked. */
        memcpy(r, base, sizeof(r));
        r[3] = 0xff;
        CHECK_EQ(decode(chip, r, 0, &dt), 0);
    }
}

TEST(arguments)
{
    datetime_t dt;
    uint8_t r[RTC_TIME_REGS];

    CHECK_EQ(decode(2, base, 0, &dt), RTC_ERR_ARG);
    CHECK_EQ(decode(RTC_CHIP_NONE, base, 0, &dt), RTC_ERR_ARG);
    CHECK_EQ(rtc_regs_decode(RTC_CHIP_DS1307, rc_launder(NULL), 0, &dt), RTC_ERR_ARG);
    CHECK_EQ(rtc_regs_decode(RTC_CHIP_DS1307, base, 0, rc_launder(NULL)), RTC_ERR_ARG);
    CHECK_EQ(rtc_regs_encode(rc_launder(NULL), r), RTC_ERR_ARG);
    dt.year = 1999;
    dt.month = 1;
    dt.day = 1;
    dt.hour = dt.minute = dt.second = 0;
    CHECK_EQ(rtc_regs_encode(&dt, r), RTC_ERR_ARG);
}

TEST(encode_round_trip)
{
    datetime_t in, out;
    uint8_t r[RTC_TIME_REGS];

    in.year = 2099;
    in.month = 12;
    in.day = 31;
    in.hour = 23;
    in.minute = 59;
    in.second = 58;
    in.weekday = 0;
    CHECK_EQ(rtc_regs_encode(&in, r), 0);
    CHECK_EQ(r[0], 0x58);            /* clock halt clear */
    CHECK_EQ(r[2], 0x23);            /* 24-hour mode */
    CHECK_EQ(r[3], 5);               /* Thursday, 1 is Sunday */
    CHECK_EQ(r[5], 0x12);            /* century bit clear */
    CHECK_EQ(r[6], 0x99);
    CHECK_EQ(decode(RTC_CHIP_DS1307, r, 0, &out), 0);
    CHECK_EQ(out.year, 2099);
    CHECK_EQ(out.second, 58);
    CHECK_EQ(decode(RTC_CHIP_DS3231, r, 0, &out), 0);
    CHECK_EQ(out.hour, 23);
}
