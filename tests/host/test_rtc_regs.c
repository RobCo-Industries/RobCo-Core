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

/* ---- rtc_detect(): read-only chip detection ---- */

/* A DS3231 read of RTC_PROBE_LEN bytes from 0x00: 0x13 repeats 0x00. */
static void ds3231_dump(uint8_t d[RTC_PROBE_LEN], const uint8_t *t0, const uint8_t *t1)
{
    static const uint8_t mid[] = {
        0x00, 0x00, 0x00, 0x01,     /* 0x07 alarm 1 */
        0x00, 0x00, 0x01,           /* 0x0b alarm 2 */
        0x1c,                       /* 0x0e control */
        0x88,                       /* 0x0f status: OSF and EN32kHz */
        0x00,                       /* 0x10 aging */
        0x19, 0x40,                 /* 0x11 0x12 temperature 25.25 C */
    };

    memcpy(d, t0, RTC_TIME_REGS);
    memcpy(d + RTC_TIME_REGS, mid, sizeof(mid));
    memcpy(d + 0x13, t1, RTC_TIME_REGS);
}

/* A DS1307 read: control at 0x07, then RAM from 0x08. */
static void ds1307_dump(uint8_t d[RTC_PROBE_LEN], uint32_t seed)
{
    unsigned i;

    memcpy(d, base, RTC_TIME_REGS);
    d[7] = 0x10;
    for (i = 8; i < RTC_PROBE_LEN; i++) {
        seed = seed * 1103515245u + 12345u;
        d[i] = (uint8_t)(seed >> 16);
    }
}

TEST(detect_ds3231)
{
    uint8_t d[RTC_PROBE_LEN];

    ds3231_dump(d, base, base);
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS3231);
}

TEST(detect_ds3231_tick_during_read)
{
    uint8_t d[RTC_PROBE_LEN];
    uint8_t t1[RTC_TIME_REGS];

    memcpy(t1, base, sizeof(t1));
    t1[0] = 0x27;
    ds3231_dump(d, base, t1);
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS3231);
    /* Two seconds is not a tick. */
    t1[0] = 0x28;
    ds3231_dump(d, base, t1);
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
    /* Backwards is not a tick. */
    ds3231_dump(d, t1, base);
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
}

TEST(detect_ds3231_tick_59_to_00)
{
    static const uint8_t t0[RTC_TIME_REGS] = { 0x59, 0x59, 0x23, 0x01, 0x31, 0x12, 0x26 };
    static const uint8_t t1[RTC_TIME_REGS] = { 0x00, 0x00, 0x00, 0x02, 0x01, 0x01, 0x27 };
    uint8_t d[RTC_PROBE_LEN];
    uint8_t m[RTC_TIME_REGS];

    ds3231_dump(d, t0, t1);
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS3231);
    /* Seconds 59 to 00 within the same hour. */
    memcpy(m, base, sizeof(m));
    m[0] = 0x59;
    memcpy(d, m, sizeof(m));
    m[0] = 0x00;
    m[1] = 0x10;
    memcpy(d + 0x13, m, sizeof(m));
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS3231);
}

TEST(detect_ds1307_random_ram)
{
    uint8_t d[RTC_PROBE_LEN];
    uint32_t seed;

    for (seed = 1; seed < 200; seed++) {
        ds1307_dump(d, seed);
        CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
    }
}

TEST(detect_ds1307_ram_matches_by_chance)
{
    uint8_t d[RTC_PROBE_LEN];

    /* RAM at 0x13 holds a copy of the time, but 0x12 and 0x0f rule it out. */
    ds1307_dump(d, 7);
    memcpy(d + 0x13, base, RTC_TIME_REGS);
    d[0x0f] = 0x00;
    d[0x12] = 0x41;
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
    d[0x12] = 0x40;
    d[0x0f] = 0x10;
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
    /* Only the seconds match: the rest of the copy rules it out. */
    ds1307_dump(d, 9);
    d[0x0f] = 0x00;
    d[0x12] = 0x00;
    d[0x13] = base[0];
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
}

TEST(detect_qemu_ds1338_zero_ram)
{
    uint8_t d[RTC_PROBE_LEN];

    /* QEMU's model starts with RAM zero, even at second 00. */
    memset(d, 0, sizeof(d));
    memcpy(d, base, RTC_TIME_REGS);
    d[0] = 0x00;
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
    d[0] = 0x59;
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
}

TEST(detect_ds3231_invalid_time_is_ds1307)
{
    uint8_t d[RTC_PROBE_LEN];
    uint8_t t[RTC_TIME_REGS];

    memcpy(t, base, sizeof(t));
    t[5] = 0x00;
    ds3231_dump(d, t, t);
    CHECK_EQ(rtc_detect(d), RTC_CHIP_DS1307);
}

TEST(detect_dead_device)
{
    uint8_t d[RTC_PROBE_LEN];

    memset(d, 0x00, sizeof(d));
    CHECK_EQ(rtc_detect(d), RTC_CHIP_NONE);
    memset(d, 0xff, sizeof(d));
    CHECK_EQ(rtc_detect(d), RTC_CHIP_NONE);
    CHECK_EQ(rtc_detect(rc_launder(NULL)), RTC_CHIP_NONE);
}
