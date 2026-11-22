#include <stdint.h>

#include <robco/datetime.h>
#include <robco/i2c.h>
#include <robco/klibc.h>
#include <robco/rtc.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define NVRAM_REG   0x08u   /* DS1307 battery-backed RAM, 56 bytes */
#define NVRAM_LEN   40u     /* more than the 16-byte FIFO */

static void iso(const datetime_t *dt, char *buf)
{
    if (dt_format_iso(dt, buf, DT_ISO_SIZE) < 0) {
        k_strncpy(buf, "invalid", DT_ISO_SIZE);
    }
}

static void scan(void)
{
    char list[160];
    size_t n = 0;
    unsigned a;
    int errors = 0;
    int rc;

    list[0] = '\0';
    for (a = 0x08; a <= 0x77; a++) {
        rc = i2c_probe((uint8_t)a);
        if (rc < 0) {
            errors++;
        } else if (rc == 1 && n + 6 < sizeof(list)) {
            n += (size_t)k_snprintf(list + n, sizeof(list) - n, "%s0x%02x", n ? "," : "", a);
        }
    }
    k_printf("@RC stage=rtc ev=scan found=%s errors=%d\n", list, errors);
}

/* Error codes, then a good transfer to show the bus recovered. */
static void errors(void)
{
    uint8_t b = 0;
    int zero = i2c_write(0x68, &b, 0);
    int addr = i2c_write(0x80, &b, 1);
    int nack = i2c_read(0x10, &b, 1);
    int nack2 = i2c_write(0x10, &b, 1);
    int after = i2c_probe(0x10);

    k_printf("@RC stage=rtc ev=errors zero=%d addr=%d nack=%d nack2=%d after=%d str=\"%s\"\n",
             zero, addr, nack, nack2, after, i2c_strerror(nack));
}

static void nvram(void)
{
    uint8_t w[NVRAM_LEN + 1];
    uint8_t r[NVRAM_LEN];
    uint8_t ptr = NVRAM_REG;
    unsigned i;
    int wrc;
    int rrc;
    int same = 1;

    w[0] = NVRAM_REG;
    for (i = 0; i < NVRAM_LEN; i++) {
        w[i + 1] = (uint8_t)(0xa5u ^ (i * 7u));
        r[i] = 0;
    }
    wrc = i2c_write(RTC_I2C_ADDR, w, sizeof(w));
    rrc = i2c_write_read(RTC_I2C_ADDR, &ptr, 1, r, sizeof(r));
    for (i = 0; i < NVRAM_LEN; i++) {
        if (r[i] != w[i + 1]) {
            same = 0;
        }
    }
    k_printf("@RC stage=rtc ev=nvram len=%u write=%d read=%d same=%d\n",
             NVRAM_LEN, wrc, rrc, same);
}

/* Writes 11 PM in 12-hour mode straight to the hours register. */
static void twelve_hour(void)
{
    static const uint8_t pm11[2] = { 0x02, 0x40 | 0x20 | 0x11 };
    datetime_t now;
    int wrc;
    int rc;

    k_memset(&now, 0, sizeof(now));
    wrc = i2c_write(RTC_I2C_ADDR, pm11, sizeof(pm11));
    rc = rtc_get(&now);

    k_printf("@RC stage=rtc ev=h12 write=%d rc=%d hour=%u\n", wrc, rc, (unsigned)now.hour);
}

static void print_clock(const char *ev)
{
    datetime_t now;
    char buf[DT_ISO_SIZE];
    int rc = clock_get(&now);

    if (rc == CLOCK_ERR_UNSET) {
        k_printf("@RC stage=rtc ev=%s state=unset\n", ev);
        return;
    }
    iso(&now, buf);
    k_printf("@RC stage=rtc ev=%s state=%s iso=%s\n", ev, rc == 0 ? "set" : "error", buf);
}

void robco_main(void)
{
    static const datetime_t fixed = { 2077, 10, 23, 9, 47, 5, 0 };
    datetime_t now;
    datetime_t back;
    char a[DT_ISO_SIZE];
    char b[DT_ISO_SIZE];
    int rc;
    int rrc;
    int running;

    uart_init(115200);
    k_printf("\n@RC stage=rtc ev=boot\n");

    rc = i2c_init(100000);
    k_printf("@RC stage=rtc ev=i2c rc=%d hz=%u\n", rc, (unsigned)i2c_get_hz());
    scan();
    errors();

    rc = rtc_init();
    running = rtc_present() ? rtc_running() : 0;
    k_printf("@RC stage=rtc ev=rtc present=%d running=%d rc=%d chip=%d\n", rtc_present(), running,
             rc, rtc_chip());
    print_clock("clock0");

    if (rtc_present()) {
        rc = rtc_get(&now);
        iso(&now, a);
        k_printf("@RC stage=rtc ev=time rc=%d iso=%s\n", rc, rc == 0 ? a : "none");

        nvram();
        twelve_hour();

        rc = rtc_set(&fixed);
        rrc = rtc_get(&back);
        clock_set(&fixed);
        iso(&fixed, a);
        iso(&back, b);
        k_printf("@RC stage=rtc ev=set rc=%d rrc=%d iso=%s readback=%s\n",
                 rc, rrc, a, rrc == 0 ? b : "none");

        timer_delay_ms(2000);
        rc = rtc_get(&now);
        iso(&now, a);
        k_printf("@RC stage=rtc ev=after rc=%d iso=%s\n", rc, rc == 0 ? a : "none");
        print_clock("clock");
    } else {
        k_printf("@RC stage=rtc ev=absent get=%d set=%d running=%d\n",
                 rtc_get(&now), rtc_set(&fixed), rtc_running());
        print_clock("clock");
    }
    k_printf("@RC stage=rtc ev=done\n");
    for (;;) {
        timer_delay_ms(1000);
    }
}
