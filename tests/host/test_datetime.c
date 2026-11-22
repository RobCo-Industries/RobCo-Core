#include <stdint.h>

#include <robco/datetime.h>

#include "rc_test.h"

static datetime_t mk(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    datetime_t dt;

    dt.year = (uint16_t)y;
    dt.month = (uint8_t)mo;
    dt.day = (uint8_t)d;
    dt.hour = (uint8_t)h;
    dt.minute = (uint8_t)mi;
    dt.second = (uint8_t)s;
    dt.weekday = 0;
    return dt;
}

TEST(leap_years)
{
    CHECK_EQ(dt_is_leap(2000), 1);
    CHECK_EQ(dt_is_leap(2004), 1);
    CHECK_EQ(dt_is_leap(2001), 0);
    CHECK_EQ(dt_is_leap(2100), 0);
    CHECK_EQ(dt_is_leap(1900), 0);
    CHECK_EQ(dt_days_in_month(2000, 2), 29);
    CHECK_EQ(dt_days_in_month(2001, 2), 28);
    CHECK_EQ(dt_days_in_month(2001, 4), 30);
    CHECK_EQ(dt_days_in_month(2001, 12), 31);
    CHECK_EQ(dt_days_in_month(2001, 0), 0);
    CHECK_EQ(dt_days_in_month(2001, 13), 0);
}

TEST(known_weekdays)
{
    CHECK_EQ(dt_weekday(2000, 1, 1), 6);     /* Saturday */
    CHECK_EQ(dt_weekday(2000, 2, 29), 2);    /* Tuesday */
    CHECK_EQ(dt_weekday(2000, 3, 1), 3);
    CHECK_EQ(dt_weekday(2001, 9, 11), 2);
    CHECK_EQ(dt_weekday(2024, 2, 29), 4);    /* Thursday */
    CHECK_EQ(dt_weekday(2077, 10, 23), 6);   /* Saturday */
    CHECK_EQ(dt_weekday(2099, 12, 31), 4);   /* Thursday */
    CHECK_EQ(dt_weekday(2001, 2, 29), -1);
    CHECK_EQ(dt_weekday(1999, 12, 31), -1);
    CHECK_EQ(dt_weekday(2100, 1, 1), -1);
}

TEST(boundaries)
{
    datetime_t dt = mk(2000, 1, 1, 0, 0, 0);
    datetime_t back;
    uint32_t s = 1;

    CHECK_EQ(dt_to_seconds(&dt, &s), 0);
    CHECK_EQ(s, 0);

    dt = mk(2000, 2, 29, 12, 0, 0);
    CHECK_EQ(dt_to_seconds(&dt, &s), 0);
    CHECK_EQ(s, (31u + 28u) * 86400u + 12u * 3600u);
    CHECK_EQ(dt_from_seconds(s, &back), 0);
    CHECK_EQ(back.month, 2);
    CHECK_EQ(back.day, 29);
    CHECK_EQ(back.weekday, 2);

    dt = mk(2099, 12, 31, 23, 59, 59);
    CHECK_EQ(dt_to_seconds(&dt, &s), 0);
    CHECK_EQ(s, DT_SECONDS_MAX);
    CHECK_EQ(dt_from_seconds(DT_SECONDS_MAX, &back), 0);
    CHECK_EQ(back.year, 2099);
    CHECK_EQ(back.month, 12);
    CHECK_EQ(back.day, 31);
    CHECK_EQ(back.second, 59);
    CHECK_EQ(dt_from_seconds(DT_SECONDS_MAX + 1u, &back), DT_ERR_RANGE);
    CHECK_EQ(dt_from_seconds(UINT32_MAX, &back), DT_ERR_RANGE);
}

TEST(round_trip_stride)
{
    uint64_t s;
    datetime_t dt;
    uint32_t back;
    int bad = 0;

    /* A prime stride touches every hour, minute and second over the range. */
    for (s = 0; s <= DT_SECONDS_MAX; s += 9973u) {
        if (dt_from_seconds((uint32_t)s, &dt) != 0 || dt_valid(&dt) != 0
            || dt_to_seconds(&dt, &back) != 0 || back != s
            || dt.weekday != dt_weekday(dt.year, dt.month, dt.day)) {
            bad++;
        }
    }
    CHECK_EQ(bad, 0);
}

TEST(every_day)
{
    unsigned y;
    unsigned m;
    unsigned d;
    uint32_t expect = 0;
    uint32_t s;
    datetime_t dt;
    int bad = 0;

    for (y = 2000; y <= 2099; y++) {
        for (m = 1; m <= 12; m++) {
            for (d = 1; d <= dt_days_in_month(y, m); d++) {
                dt = mk(y, m, d, 0, 0, 0);
                if (dt_to_seconds(&dt, &s) != 0 || s != expect) {
                    bad++;
                }
                expect += 86400u;
            }
        }
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(expect, DT_SECONDS_MAX + 1u);
}

TEST(invalid)
{
    datetime_t dt;
    uint32_t s;

    dt = mk(1999, 12, 31, 0, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2100, 1, 1, 0, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2001, 2, 29, 0, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2000, 0, 1, 0, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2000, 4, 31, 0, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2000, 1, 0, 0, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2000, 1, 1, 24, 0, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2000, 1, 1, 0, 60, 0);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    dt = mk(2000, 1, 1, 0, 0, 60);
    CHECK_EQ(dt_valid(&dt), DT_ERR_RANGE);
    CHECK_EQ(dt_to_seconds(&dt, &s), DT_ERR_RANGE);
    CHECK_EQ(dt_valid(rc_launder(NULL)), DT_ERR_ARG);
    CHECK_EQ(dt_to_seconds(rc_launder(NULL), &s), DT_ERR_ARG);
    dt = mk(2000, 1, 1, 0, 0, 0);
    CHECK_EQ(dt_to_seconds(&dt, rc_launder(NULL)), DT_ERR_ARG);
    CHECK_EQ(dt_from_seconds(0, rc_launder(NULL)), DT_ERR_ARG);
    /* The weekday field is not checked. */
    dt.weekday = 200;
    CHECK_EQ(dt_valid(&dt), 0);
}

TEST(fat_encoding)
{
    datetime_t dt = mk(2000, 1, 1, 0, 0, 0);
    datetime_t back;
    uint16_t d;
    uint16_t t;

    CHECK_EQ(dt_to_fat(&dt, &d, &t), 0);
    CHECK_EQ(d, (20u << 9) | (1u << 5) | 1u);
    CHECK_EQ(t, 0);

    dt = mk(2024, 5, 17, 13, 45, 31);
    CHECK_EQ(dt_to_fat(&dt, &d, &t), 0);
    CHECK_EQ(d, 0x58b1);    /* 44 << 9 | 5 << 5 | 17 */
    CHECK_EQ(t, 0x6daf);    /* 13 << 11 | 45 << 5 | 15 */
    CHECK_EQ(dt_from_fat(d, t, &back), 0);
    CHECK_EQ(back.year, 2024);
    CHECK_EQ(back.second, 30);
    CHECK_EQ(back.weekday, 5);

    dt = mk(2099, 12, 31, 23, 59, 59);
    CHECK_EQ(dt_to_fat(&dt, &d, &t), 0);
    CHECK_EQ(d, (119u << 9) | (12u << 5) | 31u);
    CHECK_EQ(t, (23u << 11) | (59u << 5) | 29u);

    CHECK_EQ(dt_from_fat(0x0021, 0, &back), DT_ERR_RANGE);  /* 1980-01-01 */
    CHECK_EQ(dt_from_fat((20u << 9) | (2u << 5) | 30u, 0, &back), DT_ERR_RANGE);
    CHECK_EQ(dt_from_fat((20u << 9) | (1u << 5) | 1u, 24u << 11, &back), DT_ERR_RANGE);
    CHECK_EQ(dt_to_fat(&dt, rc_launder(NULL), &t), DT_ERR_ARG);
}

TEST(bcd)
{
    unsigned v;
    int bad = 0;

    for (v = 0; v <= 99; v++) {
        if (dt_bcd_decode(dt_bcd_encode(v)) != (int)v) {
            bad++;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(dt_bcd_encode(59), 0x59);
    CHECK_EQ(dt_bcd_encode(100), 0xff);
    CHECK_EQ(dt_bcd_decode(0x0a), -1);
    CHECK_EQ(dt_bcd_decode(0xa0), -1);
    CHECK_EQ(dt_bcd_decode(0xff), -1);
}

TEST(iso_format)
{
    datetime_t dt = mk(2077, 10, 23, 9, 47, 5);
    char buf[DT_ISO_SIZE];
    char small[DT_ISO_SIZE - 1];

    CHECK_EQ(dt_format_iso(&dt, buf, sizeof(buf)), 19);
    CHECK_STR(buf, "2077-10-23T09:47:05");
    dt = mk(2000, 1, 1, 0, 0, 0);
    CHECK_EQ(dt_format_iso(&dt, buf, sizeof(buf)), 19);
    CHECK_STR(buf, "2000-01-01T00:00:00");
    small[0] = 'x';
    CHECK_EQ(dt_format_iso(&dt, small, sizeof(small)), DT_ERR_ARG);
    CHECK_EQ(small[0], '\0');
    dt.month = 13;
    CHECK_EQ(dt_format_iso(&dt, buf, sizeof(buf)), DT_ERR_RANGE);
    CHECK_STR(buf, "");
    CHECK_EQ(dt_format_iso(rc_launder(NULL), buf, sizeof(buf)), DT_ERR_ARG);
}
