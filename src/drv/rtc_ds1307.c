#include <stdint.h>

#include <robco/datetime.h>
#include <robco/i2c.h>
#include <robco/rtc.h>
#include <robco/timer.h>

#define REG_SECONDS     0x00
#define SEC_CH          0x80u   /* clock halt */
#define HOUR_12         0x40u   /* 12-hour mode */
#define HOUR_PM         0x20u

static int present;

static int clock_is_set;
static uint32_t clock_base_s;   /* seconds since 2000 at clock_base_us */
static uint64_t clock_base_us;

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

static int read_regs(uint8_t regs[7])
{
    uint8_t ptr = REG_SECONDS;

    if (!present) {
        return RTC_ERR_ABSENT;
    }
    return i2c_write_read(RTC_I2C_ADDR, &ptr, 1, regs, 7);
}

int rtc_init(void)
{
    datetime_t now;
    int rc;

    present = 0;
    rc = i2c_probe(RTC_I2C_ADDR);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return RTC_ERR_ABSENT;
    }
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

int rtc_get(datetime_t *out)
{
    uint8_t r[7];
    int sec, min, hour, day, mon, year;
    int rc;

    if (out == NULL) {
        return RTC_ERR_ARG;
    }
    rc = read_regs(r);
    if (rc < 0) {
        return rc;
    }
    if (r[0] & SEC_CH) {
        return RTC_ERR_HALTED;
    }
    sec = field(r[0], 0x7f, 0, 59);
    min = field(r[1], 0x7f, 0, 59);
    hour = decode_hour(r[2]);
    day = field(r[4], 0x3f, 1, 31);
    mon = field(r[5], 0x1f, 1, 12);
    year = field(r[6], 0xff, 0, 99);
    if (sec < 0 || min < 0 || hour < 0 || day < 0 || mon < 0 || year < 0
        || (r[1] & 0x80u) || (r[2] & 0x80u) || (r[4] & 0xc0u) || (r[5] & 0xe0u)) {
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

int rtc_set(const datetime_t *dt)
{
    uint8_t buf[8];
    int wd;

    if (dt == NULL || dt_valid(dt) != 0) {
        return RTC_ERR_ARG;
    }
    if (!present) {
        return RTC_ERR_ABSENT;
    }
    wd = dt_weekday(dt->year, dt->month, dt->day);
    buf[0] = REG_SECONDS;
    buf[1] = dt_bcd_encode(dt->second);     /* CH = 0 starts the oscillator */
    buf[2] = dt_bcd_encode(dt->minute);
    buf[3] = dt_bcd_encode(dt->hour);       /* bit 6 = 0: 24-hour mode */
    buf[4] = (uint8_t)(wd + 1);             /* 1 Sunday to 7 Saturday */
    buf[5] = dt_bcd_encode(dt->day);
    buf[6] = dt_bcd_encode(dt->month);
    buf[7] = dt_bcd_encode(dt->year - 2000u);
    return i2c_write(RTC_I2C_ADDR, buf, sizeof(buf));
}

int rtc_running(void)
{
    uint8_t ptr = REG_SECONDS;
    uint8_t sec;
    int rc;

    if (!present) {
        return RTC_ERR_ABSENT;
    }
    rc = i2c_write_read(RTC_I2C_ADDR, &ptr, 1, &sec, 1);
    if (rc < 0) {
        return rc;
    }
    return (sec & SEC_CH) ? 0 : 1;
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
