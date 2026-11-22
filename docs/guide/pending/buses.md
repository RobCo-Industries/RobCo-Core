# Buses, clock and configuration

These sections document `i2c.h`, `rtc.h`, `spi.h`, `datetime.h` and `ini.h`. They follow the style of [API.md](../API.md) and go into it as five new sections.

| Header | Section |
|---|---|
| `i2c.h` | [I2C master](#i2c-master-i2ch) |
| `rtc.h` | [Real-time clock and software clock](#real-time-clock-and-software-clock-rtch) |
| `spi.h` | [SPI master](#spi-master-spih) |
| `datetime.h` | [Date and time](#date-and-time-datetimeh) |
| `ini.h` | [Configuration files](#configuration-files-inih) |

Rows to add to the initialisation order table:

| Step | Call | Needs |
|---|---|---|
| after 3 | `i2c_init(100000)` | Step 1. Uses the mailbox and the GPIO API. |
| after `i2c_init()` | `rtc_init()` | `i2c_init()`. Seeds the software clock. |
| after 3 | `spi_init()`, then `spi_configure()` | Step 1. Uses the mailbox and the GPIO API. |

`datetime.h` and `ini.h` need no init.

## I2C master (`i2c.h`)

`i2c.h` drives the BSC1 controller at peripheral base + `0x804000` as an I2C master with 7-bit addresses. It uses GPIO 2 (SDA) and GPIO 3 (SCL) at ALT0. These header pins have fixed 1.8 kOhm pull-ups on the board. The driver polls; it takes no interrupt.

| Constant | Value | Meaning |
|---|---|---|
| `I2C_HZ_MIN` | `10000` | Lowest accepted rate |
| `I2C_HZ_MAX` | `1000000` | Highest accepted rate |
| `I2C_ADDR_MAX` | `0x7f` | Highest 7-bit address |
| `I2C_CORE_FALLBACK_HZ` | `250000000` | Core clock used when the mailbox does not answer |
| `I2C_ERR_ARG` | `-1` | Bad address, length, buffer or rate |
| `I2C_ERR_INIT` | `-2` | `i2c_init()` has not run or failed |
| `I2C_ERR_NACK` | `-3` | The address or a data byte was not acknowledged |
| `I2C_ERR_TIMEOUT` | `-4` | The transfer did not finish before its deadline |
| `I2C_ERR_CLKT` | `-5` | The slave held SCL low longer than the clock stretch timeout |

```c
int i2c_init(uint32_t hz);
uint32_t i2c_get_hz(void);
```

`i2c_init()` reads the core clock (mailbox tag `0x00030002`, clock id 4) and falls back to `I2C_CORE_FALLBACK_HZ`. It sets the divider to the next even value at or above `core / hz`, so the bus never runs faster than asked, and sets the clock stretch timeout to 64 SCL periods. Then it muxes GPIO 2 and 3 to ALT0. You can call it again to change the rate. `i2c_get_hz()` returns the rate after rounding, or `0` before init.

```c
int i2c_write(uint8_t addr, const void *buf, size_t len);
int i2c_read(uint8_t addr, void *buf, size_t len);
```

One transfer each: START, address, `len` bytes, STOP. `len` goes from 1 to 65535. The driver refills or drains the 16-byte FIFO while the transfer runs, so any length works.

```c
int i2c_write_read(uint8_t addr, const void *wbuf, size_t wlen, void *rbuf, size_t rlen);
```

Writes `wbuf`, then reads `rbuf`, as two transfers with a STOP and a START between them. It does not use a repeated start. That suits register-pointer devices such as RTCs, EEPROMs and most sensors. A device that loses its register pointer on STOP needs a repeated start, which this driver does not do.

```c
int i2c_probe(uint8_t addr);
```

Reads one byte from `addr`. Returns `1` if the address acknowledges, `0` if not, or a negative error. A read is harmless for most devices; it is what `i2cdetect -r` does.

```c
const char *i2c_strerror(int code);
```

Short text for an `I2C_ERR_*` code, or `"unknown error"`.

Every transfer has a deadline on the system timer: 20 ms plus four times the time the bytes take on the wire. After an error the driver clears the FIFO and the status bits, so the next transfer starts clean. Errors are distinct: a missing device gives `I2C_ERR_NACK`, a stuck bus gives `I2C_ERR_CLKT` or `I2C_ERR_TIMEOUT`.

Context: main context only. The driver has no lock, so do not call it from an IRQ handler or a tick callback.

Sharp edges:

- The divider comes from the core clock read at init. On boards where the core clock changes with load (Pi 3 without `core_freq` fixed), the bus rate changes with it. Fix the core clock in `config.txt` or call `i2c_init()` again.
- The BCM2835 BSC has a clock stretching bug: a slave that stretches SCL by a short time can corrupt the transfer. Keep slaves that stretch at low rates, or avoid them.
- In QEMU every transfer finishes at once, and the model starts a transfer on any write of the control register with I2CEN set. The driver writes I2CEN only together with ST for that reason.

```c
uint8_t reg = 0x00;
uint8_t regs[7];

i2c_init(100000);
if (i2c_probe(0x68) == 1) {
    int rc = i2c_write_read(0x68, &reg, 1, regs, sizeof(regs));
    k_printf("rc=%d %s\n", rc, i2c_strerror(rc));
}
```

Wiring: SDA on header pin 3, SCL on header pin 5, ground on pin 6. Power 3.3 V devices from pin 1. Do not add pull-ups to 5 V.

## Real-time clock and software clock (`rtc.h`)

`rtc.h` reads and sets a DS1307 at I2C address `0x68`, and keeps a software clock for boards without one. The DS1338 and DS1340 share the DS1307 time registers. The DS1337 and DS3231 share registers 0 to 6 but have no clock-halt bit and use bit 7 of the month as a century flag; `rtc_get()` rejects that bit, so they are not supported. The chip holds UTC by convention.

| Constant | Value | Meaning |
|---|---|---|
| `RTC_I2C_ADDR` | `0x68` | Device address |
| `RTC_ERR_ARG` | `-10` | NULL, or a date and time that `dt_valid()` rejects |
| `RTC_ERR_ABSENT` | `-11` | Nothing acknowledges `0x68` |
| `RTC_ERR_HALTED` | `-12` | The oscillator is stopped: the time is not valid |
| `RTC_ERR_DATA` | `-13` | A register holds something that is not a date or time |
| `CLOCK_ERR_ARG` | `-1` | NULL |
| `CLOCK_ERR_UNSET` | `-2` | `clock_set()` never ran |

The RTC functions also pass `I2C_ERR_*` codes through. The two ranges do not overlap.

```c
int rtc_init(void);
int rtc_present(void);
```

`rtc_init()` probes `0x68`. It returns `0` when the chip answers, `RTC_ERR_ABSENT` when nothing does, or an I2C error. When the chip runs and holds a valid time, it also calls `clock_set()` with that time. `rtc_present()` returns `1` after `rtc_init()` found the chip, else `0`. Every other RTC call returns `RTC_ERR_ABSENT` until then.

```c
int rtc_get(datetime_t *out);
```

Reads registers 0 to 6 in one transfer, so the fields belong to the same second. Returns `RTC_ERR_HALTED` when the clock-halt bit is set. Decodes 12-hour mode into 0 to 23. Returns `RTC_ERR_DATA` for a BCD digit above 9, a field out of range, a day past the end of the month, or a reserved bit set. It never returns a half-valid time. The weekday comes from the date; the chip's weekday register is ignored.

```c
int rtc_set(const datetime_t *dt);
int rtc_running(void);
```

`rtc_set()` writes all seven time registers in one transfer, in 24-hour mode, with the clock-halt bit clear, so the oscillator starts. It writes the weekday register as 1 (Sunday) to 7. It does not touch the control register or the RAM. `rtc_running()` returns `1` when the oscillator runs, `0` when halted, or a negative error.

```c
void clock_set(const datetime_t *dt);
int clock_get(datetime_t *out);
```

The software clock. `clock_set()` stores the time and the system timer value; an invalid time leaves the clock as it was. `clock_get()` adds the whole seconds since then and returns `0`, or `CLOCK_ERR_UNSET` before any `clock_set()`. It needs no tick and no interrupt. It stops at 2099-12-31 23:59:59. Call `clock_set()` after `rtc_set()` to keep them in step.

Context: main context only for the RTC calls. `clock_get()` reads a 64-bit state that `clock_set()` writes, so call `clock_set()` from one context only.

Sharp edges:

- The software clock drifts with the system timer crystal, about the same as the RTC. It does not survive a reset; `rtc_init()` seeds it again.
- A new DS1307 starts halted with garbage in its registers. `rtc_get()` returns `RTC_ERR_HALTED` until `rtc_set()` runs.
- The DS1307 needs 5 V and its I2C lines then idle at 5 V. Use a module with 3.3 V pull-ups or a DS1338 or DS3231-class part with 3.3 V supply.

```c
datetime_t now;
char iso[DT_ISO_SIZE];

i2c_init(100000);
rtc_init();
if (clock_get(&now) == 0 && dt_format_iso(&now, iso, sizeof(iso)) > 0) {
    k_printf("now %s\n", iso);
}
```

`examples/rtc/main.c` scans the bus, reads, sets and reads back the time.

## SPI master (`spi.h`)

`spi.h` drives SPI0 at peripheral base + `0x204000` as a polled master with 8-bit words, MSB first. Pins at ALT0: GPIO 8 (CE0), GPIO 7 (CE1), GPIO 9 (MISO), GPIO 10 (MOSI), GPIO 11 (SCLK).

| Constant | Value | Meaning |
|---|---|---|
| `SPI_CS_COUNT` | `2` | CE0 and CE1 |
| `SPI_MODE_0` to `SPI_MODE_3` | `0` to `3` | CPOL in bit 1, CPHA in bit 0 |
| `SPI_CS_HIGH` | `4` | ORed into the mode: chip select active high |
| `SPI_DEFAULT_HZ` | `1000000` | Rate after `spi_init()` |
| `SPI_CORE_FALLBACK_HZ` | `250000000` | Core clock used when the mailbox does not answer |
| `SPI_ERR_ARG` | `-1` | Bad chip select, rate, mode or buffer |
| `SPI_ERR_INIT` | `-2` | `spi_init()` has not run or failed |
| `SPI_ERR_TIMEOUT` | `-3` | The controller did not finish before its deadline |
| `SPI_ERR_BUSY` | `-4` | Another chip select is between `spi_begin()` and `spi_end()` |

```c
int spi_init(void);
```

Muxes the five pins, reads the core clock (clock id 4) and sets both chip selects to `SPI_DEFAULT_HZ`, mode 0, active low.

```c
int spi_configure(unsigned cs, uint32_t hz, unsigned mode);
uint32_t spi_get_hz(unsigned cs);
```

Sets the rate and mode of one chip select. The divider rounds up to an even value, so the clock never runs faster than `hz`. The slowest rate is the core clock / 65534, about 3.8 kHz. The new settings apply at the next `spi_begin()` or transfer. `spi_configure()` returns `SPI_ERR_BUSY` while a transaction is open, because the chip select polarity bits are shared. `spi_get_hz()` returns the rate after rounding.

```c
int spi_transfer(unsigned cs, const void *tx, void *rx, size_t len);
int spi_write(unsigned cs, const void *tx, size_t len);
```

`spi_transfer()` clocks `len` bytes full duplex. With `tx` NULL it sends zeros; with `rx` NULL it drops what comes in. Chip select stays asserted for the whole call. `len` 0 returns `0` and does nothing. `spi_write()` is `spi_transfer()` with `rx` NULL; its `tx` must not be NULL. Each call has a deadline of 20 ms plus four times the time the bytes take on the wire. A timeout clears the FIFOs and releases chip select, also inside a `spi_begin()` pair.

```c
int spi_begin(unsigned cs);
int spi_end(unsigned cs);
```

`spi_begin()` asserts `cs` and keeps it asserted across later transfers on the same `cs`, until `spi_end()`. Between them you can change GPIOs, for example a display's data/command pin. A call for the other chip select returns `SPI_ERR_BUSY`. `spi_begin()` on the open `cs` returns `0`. `spi_end()` waits for the last byte, then releases chip select; with no open transaction it returns `0`.

Context: main context only. No lock.

Sharp edges:

- The rate depends on the core clock read at init, as for I2C.
- The driver keeps at most 16 bytes in flight, so it never overruns the RX FIFO. Long transfers run at the bus rate, but with a short gap every 16 bytes.
- QEMU's model ignores the rate, the mode and chip select, and shifts in zeros when no device is attached.

```c
spi_init();
spi_configure(0, 8000000, SPI_MODE_0);
spi_begin(0);
gpio_write(DC_PIN, 0);
spi_write(0, &cmd, 1);
gpio_write(DC_PIN, 1);
spi_write(0, pixels, sizeof(pixels));
spi_end(0);
```

Wiring: MOSI on header pin 19, MISO on pin 21, SCLK on pin 23, CE0 on pin 24, CE1 on pin 26. All lines are 3.3 V.

## Date and time (`datetime.h`)

`datetime.h` is pure code in `src/lib/datetime.c`, host-tested. It covers 2000-01-01 00:00:00 to 2099-12-31 23:59:59, the range of a two-digit RTC year. It has no time zones and no floating point.

```c
typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second, weekday;
} datetime_t;
```

Month 1 to 12, day 1 to the month's length, hour 0 to 23, weekday 0 (Sunday) to 6.

| Constant | Value | Meaning |
|---|---|---|
| `DT_YEAR_MIN`, `DT_YEAR_MAX` | `2000`, `2099` | Accepted years |
| `DT_SECONDS_MAX` | `3155759999` | Seconds from 2000-01-01 to 2099-12-31 23:59:59 |
| `DT_ISO_SIZE` | `20` | Buffer size for `dt_format_iso()` |
| `DT_ERR_ARG` | `-1` | NULL pointer or buffer too small |
| `DT_ERR_RANGE` | `-2` | Field or value out of range |

```c
int dt_is_leap(unsigned year);
unsigned dt_days_in_month(unsigned year, unsigned month);
int dt_weekday(unsigned year, unsigned month, unsigned day);
```

Gregorian leap rule for any year. `dt_days_in_month()` returns `0` for a month outside 1 to 12. `dt_weekday()` returns 0 to 6, or `-1` for a date outside the range or invalid.

```c
int dt_valid(const datetime_t *dt);
```

Returns `0` when every field except `weekday` is in range. The weekday is not checked, because the other fields fix it.

```c
int dt_to_seconds(const datetime_t *dt, uint32_t *out);
int dt_from_seconds(uint32_t seconds, datetime_t *out);
```

Converts to and from seconds since 2000-01-01 00:00:00. `dt_from_seconds()` fills in the weekday and returns `DT_ERR_RANGE` above `DT_SECONDS_MAX`.

```c
int dt_to_fat(const datetime_t *dt, uint16_t *fat_date, uint16_t *fat_time);
int dt_from_fat(uint16_t fat_date, uint16_t fat_time, datetime_t *out);
```

FAT directory stamps. Date: bits 15 to 9 hold the year minus 1980, bits 8 to 5 the month, bits 4 to 0 the day. Time: bits 15 to 11 the hour, 10 to 5 the minute, 4 to 0 the second divided by 2, so odd seconds round down. `dt_from_fat()` rejects stamps before 2000 or with invalid fields.

```c
uint8_t dt_bcd_encode(unsigned value);
int dt_bcd_decode(uint8_t bcd);
```

Two BCD digits. `dt_bcd_encode()` returns `0xff` above 99. `dt_bcd_decode()` returns `-1` when a digit is above 9.

```c
int dt_format_iso(const datetime_t *dt, char *buf, size_t size);
```

Writes `YYYY-MM-DDTHH:MM:SS` and a NUL, with no zone suffix. Returns `19`, or a negative error with `buf` set to `""`.

All functions are reentrant and safe in any context.

```c
datetime_t dt;
uint16_t d, t;

dt_from_seconds(0, &dt);         /* 2000-01-01T00:00:00, Saturday */
dt_to_fat(&dt, &d, &t);          /* d = 0x2821, t = 0 */
```

## Configuration files (`ini.h`)

`ini.h` is a strict INI parser for text already in memory, such as a file read from the SD card. It is pure code in `src/lib/ini.c`, host-tested. It does not allocate and keeps no global state, so it is reentrant. It does not write INI files.

Syntax, one item per line:

| Line | Meaning |
|---|---|
| `[name]` | Starts a section. Spaces inside the brackets around the name are allowed. A comment may follow. |
| `key = value` | A key in the current section. Keys before any section belong to the section `""`. |
| `# text` or `; text` | Comment |
| blank, or spaces and tabs only | Ignored |

Rules:

- Section names and keys use `A` to `Z`, `a` to `z`, `0` to `9`, `_`, `.` and `-`. They are case-sensitive.
- Lines end in LF or CR LF. The last line may lack a line end.
- Spaces and tabs around names and values are dropped.
- Comment rule for unquoted values: a `#` or `;` starts a trailing comment when it is the first character of the value or follows a space or tab. So `a = b#c` gives `b#c`, `a = b #c` gives `b`, and `color = #f00` gives an empty value. Quote a value that must start with `#` or contain ` #`.
- A value in double quotes keeps everything inside, with the escapes `\"`, `\\` and `\n`. Any other escape is an error. Only spaces and a comment may follow the closing quote.
- NUL, a lone CR, DEL and control bytes other than tab are errors. Bytes 0x80 and above pass through, so UTF-8 works in values.

| Limit | Value |
|---|---|
| `INI_MAX_LINE` | 255 bytes per line, without the line end |
| `INI_MAX_SECTION` | 31 bytes per section name |
| `INI_MAX_KEY` | 31 bytes per key |
| `INI_MAX_VALUE` | 127 bytes per value, after escapes |

A line, name or value above its limit is an error. Nothing is ever cut short.

| Code | Value | Meaning |
|---|---|---|
| `INI_STOPPED` | `1` | The handler returned non-zero |
| `INI_ERR_ARG` | `-1` | NULL text with a length, no handler, or a bad helper argument |
| `INI_ERR_SYNTAX` | `-2` | A line that is not a section, key or comment |
| `INI_ERR_TOO_LONG` | `-3` | Line, name or value above its limit |
| `INI_ERR_CHAR` | `-4` | NUL, lone CR or other control byte |
| `INI_ERR_NOT_FOUND` | `-5` | No such key |
| `INI_ERR_SPACE` | `-6` | The value does not fit the caller's buffer |
| `INI_ERR_VALUE` | `-7` | Not a number, or not a boolean word |
| `INI_ERR_RANGE` | `-8` | Number outside the caller's range |

```c
typedef int (*ini_handler_t)(const char *section, const char *key,
                             const char *value, unsigned line, void *ctx);
int ini_parse(const char *text, size_t len, ini_handler_t fn, void *ctx, unsigned *err_line);
```

Calls `fn` once per key, in file order, with NUL-terminated copies that live until `fn` returns. `line` counts from 1. Return `0` from `fn` to go on, anything else to stop. `ini_parse()` returns `0` at the end, `INI_STOPPED` when `fn` stopped it, or an error. `err_line`, if not NULL, gets the line of the error, or `0`. Keys before the bad line have already reached `fn`, so check the return code before you use them. The scratch buffers, about 200 bytes, live on the stack.

```c
int ini_get(const char *text, size_t len, const char *section, const char *key,
            char *out, size_t out_size);
```

Copies the value of `key` in `section` into `out`. When a key appears twice in a section, the last one wins; the callback sees both. The whole text must parse, so a syntax error anywhere fails the lookup. Each call parses the whole text again.

```c
int ini_get_int(const char *text, size_t len, const char *section, const char *key,
                int32_t min, int32_t max, int32_t *out);
int ini_get_bool(const char *text, size_t len, const char *section, const char *key,
                 int *out);
```

`ini_get_int()` accepts decimal with an optional `+` or `-`, or `0x` hex without a sign, and checks `min <= value <= max`. `ini_get_bool()` accepts `true`/`false`, `yes`/`no`, `on`/`off` and `1`/`0`, in any case, and stores `1` or `0`. `*out` changes only on success.

```c
const char *ini_strerror(int code);
```

Short text for an `INI_*` code.

All functions are reentrant and safe in any context, but a large file takes time to parse: keep it out of IRQ context.

```c
static const char conf[] = "[display]\nwidth = 640\nscanlines = on\n";
int32_t width;
int scan;

if (ini_get_int(conf, sizeof(conf) - 1, "display", "width", 320, 1920, &width) == 0
    && ini_get_bool(conf, sizeof(conf) - 1, "display", "scanlines", &scan) == 0) {
    k_printf("width=%d scanlines=%d\n", (int)width, scan);
}
```
