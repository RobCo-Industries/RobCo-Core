# Buses, clock and configuration

This page covers the I2C and SPI masters, the DS1307, DS1338 and DS3231 real-time clocks with the software clock, date and time arithmetic, and the INI parser. Return codes and context rules follow the [Conventions](../API.md#conventions).

## I2C master (`i2c.h`)

`i2c.h` drives the BSC1 controller at peripheral base + `0x804000` as an I2C master with 7-bit addresses. It uses GPIO 2 (SDA) and GPIO 3 (SCL) at ALT0. These header pins have fixed 1.8 kOhm pull-ups on the board. The driver polls and takes no interrupt.

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

`i2c_init()` returns `I2C_ERR_ARG` for a rate outside `I2C_HZ_MIN` to `I2C_HZ_MAX`. It reads the core clock (mailbox clock id 4) and falls back to `I2C_CORE_FALLBACK_HZ`. The divider is `core / hz` rounded up, then rounded up to an even value, at least 2. So the bus never runs faster than asked. It muxes GPIO 2 and 3 to ALT0, writes the divider and sets the clock stretch timeout to 64 SCL periods. It returns `I2C_ERR_INIT` when the board is unknown or the pins cannot be muxed. You can call it again to change the rate. `i2c_get_hz()` returns `core / divider`, or `0` before init.

```c
int i2c_write(uint8_t addr, const void *buf, size_t len);
int i2c_read(uint8_t addr, void *buf, size_t len);
```

One transfer each: START, address, `len` bytes, STOP. `len` goes from 1 to 65535. The driver refills or drains the 16-byte FIFO while the transfer runs, so any length in that range works.

```c
int i2c_write_read(uint8_t addr, const void *wbuf, size_t wlen, void *rbuf, size_t rlen);
```

Writes `wbuf`, then reads `rbuf`, as two transfers with a STOP and a START between them. It does not use a repeated start. That suits register-pointer devices such as RTCs, EEPROMs and most sensors. A device that loses its register pointer on STOP needs a repeated start, which this driver does not do. Both lengths must be at least 1. An error on the write skips the read.

```c
int i2c_probe(uint8_t addr);
```

Reads one byte from `addr`. Returns `1` if the address acknowledges, `0` on `I2C_ERR_NACK`, or another negative error. A read is harmless for most devices. It is what `i2cdetect -r` does.

```c
const char *i2c_strerror(int code);
```

Short text for `0` or an `I2C_ERR_*` code, or `"unknown error"`.

Every transfer has a deadline on the system timer: 20 ms plus four times the wire time of `(len + 2) * 9` bits at the rounded rate. After every transfer, good or bad, the driver clears the FIFO and the status bits, so the next transfer starts clean. After an error it first waits up to 1 ms for the controller to send STOP. A missing device gives `I2C_ERR_NACK`. A stuck bus gives `I2C_ERR_CLKT` or `I2C_ERR_TIMEOUT`.

Context: main context only. The driver has no lock, so do not call it from an IRQ handler or a tick callback.

| Sharp edge | Detail |
|---|---|
| Core clock | The divider comes from the core clock read at init. On boards where the core clock changes with load (Pi 3 without a fixed `core_freq`), the bus rate changes with it. Fix the core clock in `config.txt` or call `i2c_init()` again. |
| Clock stretching | The BCM2835 BSC has a clock stretching bug: a slave that stretches SCL briefly can corrupt the transfer. Keep slaves that stretch at low rates, or avoid them. |
| QEMU | Every transfer finishes at once. The model starts a transfer on any write of the control register with I2CEN set, so the driver writes I2CEN only together with ST. |

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

Clock stretching, real rates and the bus timeouts are pending hardware validation. See [hardware validation](../../HARDWARE_VALIDATION.md).

## Real-time clock and software clock (`rtc.h`)

`rtc.h` reads and sets a DS1307, DS1338 or DS3231 at I2C address `0x68`, and keeps a software clock for boards without one. The chips share the time registers `0x00` to `0x06`. The DS1307 and DS1338 stop the clock with bit 7 of the seconds, the clock-halt bit. The DS3231 has no such bit, uses bit 7 of the month as a century bit, and flags a stopped oscillator in bit 7 of its status register `0x0F`. The chip holds UTC by convention. DS3231 support is pending hardware validation.

| Constant | Value | Meaning |
|---|---|---|
| `RTC_I2C_ADDR` | `0x68` | Device address |
| `RTC_ERR_ARG` | `-10` | NULL, or a date and time that `dt_valid()` rejects |
| `RTC_ERR_ABSENT` | `-11` | Nothing acknowledges `0x68`, or `rtc_init()` did not find the chip |
| `RTC_ERR_HALTED` | `-12` | The oscillator is stopped: the time is not valid |
| `RTC_ERR_DATA` | `-13` | A register holds something that is not a date or time |
| `RTC_CHIP_AUTO` | `-2` | `rtc_force_chip()` and `ROBCO_RTC_CHIP`: detect the chip |
| `RTC_CHIP_NONE` | `-1` | `rtc_init()` found no chip |
| `RTC_CHIP_DS1307` | `0` | DS1307 or DS1338 |
| `RTC_CHIP_DS3231` | `1` | DS3231 |
| `RTC_TIME_REGS` | `7u` | Time registers `0x00` to `0x06` |
| `RTC_DS3231_STATUS` | `0x0fu` | DS3231 status register |
| `RTC_DS3231_OSF` | `0x80u` | Oscillator-stop flag in the DS3231 status register |
| `RTC_PROBE_LEN` | `0x1au` | Bytes the detection reads from register `0x00` |
| `CLOCK_ERR_ARG` | `-1` | NULL |
| `CLOCK_ERR_UNSET` | `-2` | `clock_set()` never ran |

The RTC functions also pass `I2C_ERR_*` codes through. The two ranges do not overlap.

```c
int rtc_init(void);
int rtc_present(void);
int rtc_chip(void);
```

`rtc_init()` probes `0x68`. It needs `i2c_init()` first. It returns `0` when the chip answers, `RTC_ERR_ABSENT` when nothing does, or an I2C error such as `I2C_ERR_INIT`. Then it detects the chip with reads only. It never writes the chip: another device may answer at `0x68`, and the DS1307 RAM belongs to the user. When the chip runs and `rtc_get()` succeeds, it also calls `clock_set()` with that time. `rtc_present()` returns `1` after `rtc_init()` found the chip, else `0`. Every other RTC call returns `RTC_ERR_ABSENT` until then. `rtc_chip()` returns the `RTC_CHIP_*` code found, or `RTC_CHIP_NONE`.

Detection rule. `rtc_init()` reads `RTC_PROBE_LEN` bytes in one transfer from register `0x00`. The DS3231 register pointer wraps from `0x12` to `0x00`, so bytes `0x13` to `0x19` repeat the time registers. On a DS1307 or DS1338 they are RAM. `rtc_detect()` returns `RTC_CHIP_DS3231` only when all of these hold:

- Both copies of the time registers decode as a valid DS3231 time, and the second copy is the same time or one second later, for a tick during the read.
- Bits 6 to 4 of the status register `0x0F` read `0`.
- Bits 5 to 0 of the temperature register `0x12` read `0`.

When every byte reads `0x00`, or every byte reads `0xFF`, the device is dead or the bus is stuck: `rtc_init()` returns `RTC_ERR_ABSENT` and `rtc_present()` stays `0`. Anything else means `RTC_CHIP_DS1307`. A DS3231 with an invalid time also lands there. `rtc_set()` then writes a valid time, and the next `rtc_init()` finds the DS3231.

```c
int rtc_force_chip(int chip);
```

Skips the detection for the next `rtc_init()`. Takes `RTC_CHIP_AUTO`, `RTC_CHIP_DS1307` or `RTC_CHIP_DS3231`, and returns `0` or `RTC_ERR_ARG`. It overrides `ROBCO_RTC_CHIP`. `rtc_init()` still reads the registers once, and still returns `RTC_ERR_ABSENT` for an all `0x00` or all `0xFF` answer.

| Macro | Default | Meaning |
|---|---|---|
| `ROBCO_RTC_CHIP` | `RTC_CHIP_AUTO` | Set in `robco_config.h` to `RTC_CHIP_DS1307` or `RTC_CHIP_DS3231` when the product knows its chip. Any other value stops the build. |

```c
int rtc_get(datetime_t *out);
```

Reads registers 0 to 6 in one transfer, so the fields belong to the same second. It never writes. On a DS3231 it also reads the status register. Returns `RTC_ERR_HALTED` when the clock-halt bit is set on a DS1307 or DS1338, or the oscillator-stop flag on a DS3231. Decodes 12-hour mode into 0 to 23. Ignores the DS3231 century bit. Returns `RTC_ERR_DATA` for a BCD digit above 9, a field out of range, a day past the end of the month, or a reserved bit set in the seconds of a DS3231, or in the minute, hour, day or month register. It never returns a half-valid time. The weekday comes from the date. The chip's weekday register is ignored.

```c
int rtc_set(const datetime_t *dt);
int rtc_running(void);
```

`rtc_set()` is the only RTC call that writes the chip. It writes all seven time registers in one transfer, in 24-hour mode, with the clock-halt and century bits clear, so the oscillator starts. It writes the weekday register as 1 (Sunday) to 7. On a DS3231 it then reads the status register and writes it back with only the oscillator-stop flag cleared. `EN32kHz` and the alarm flags keep their value. An alarm flag that rises between the read and the write is lost. It does not touch the control register, the DS1338 register `0x07` or the RAM. `rtc_init()`, `rtc_get()` and `rtc_running()` only read. It checks `dt` before presence, so a bad `dt` gives `RTC_ERR_ARG` even without a chip. `rtc_running()` returns `1` when the oscillator runs, `0` when halted, or a negative error. It reads the clock-halt bit, or the oscillator-stop flag on a DS3231.

```c
void clock_set(const datetime_t *dt);
int clock_get(datetime_t *out);
```

The software clock. `clock_set()` stores the time and the system timer value. NULL or an invalid time leaves the clock as it was. `clock_get()` adds the whole seconds since then and returns `0`, `CLOCK_ERR_ARG` for NULL, or `CLOCK_ERR_UNSET` before any `clock_set()`. It needs no tick and no interrupt. It stops at 2099-12-31 23:59:59. Call `clock_set()` after `rtc_set()` to keep them in step.

Context: main context only for the RTC calls. `clock_get()` reads a 64-bit state that `clock_set()` writes, so call `clock_set()` from one context only.

| Sharp edge | Detail |
|---|---|
| Drift | The software clock drifts with the system timer crystal, about as much as the RTC. It does not survive a reset. `rtc_init()` seeds it again. |
| New chip | A new DS1307 starts halted with garbage in its registers, and a new DS3231 with its oscillator-stop flag set. `rtc_get()` returns `RTC_ERR_HALTED` until `rtc_set()` runs. |
| 5 V | The DS1307 needs 5 V, and many modules then pull the I2C lines to 5 V. Use a module with 3.3 V pull-ups, or a DS1338 with a 3.3 V supply. |
| QEMU | The `ds1338` model ignores the clock-halt bit and follows the host clock until the guest sets it. `RTC_ERR_HALTED` is tested only on the host. QEMU has no DS3231 model. |
| DS3231 | A DS3231 with an invalid time, or with the clock running backwards, reads as `RTC_CHIP_DS1307` until `rtc_set()` runs. Set `ROBCO_RTC_CHIP` when the product knows its chip. |
| DS1307 RAM | A DS1307 or DS1338 is read as a DS3231 only if its RAM at `0x13` holds a valid copy of the current time and bytes `0x0F` and `0x12` have the DS3231 zero bits. |
| File time stamps | `fs.h` stamps new and modified files with the software clock once it is set. See [File time stamps](./storage.md#file-time-stamps). |

```c
datetime_t now;
char iso[DT_ISO_SIZE];

i2c_init(100000);
rtc_init();
if (clock_get(&now) == 0 && dt_format_iso(&now, iso, sizeof(iso)) > 0) {
    k_printf("now %s\n", iso);
}
```

```c
int rtc_regs_decode(int chip, const uint8_t regs[RTC_TIME_REGS], uint8_t status,
                    datetime_t *out);
int rtc_regs_encode(const datetime_t *dt, uint8_t regs[RTC_TIME_REGS]);
```

The register codec behind `rtc_get()` and `rtc_set()`, pure in `src/lib/rtc_regs.c` and tested on the host. `rtc_regs_decode()` applies the rules of `rtc_get()` to registers 0 to 6 of `chip`. `status` is the DS3231 status register, ignored for `RTC_CHIP_DS1307`. It returns `0`, `RTC_ERR_HALTED`, `RTC_ERR_DATA`, or `RTC_ERR_ARG` for a NULL pointer or an unknown chip. `rtc_regs_encode()` fills the seven registers as `rtc_set()` writes them, and returns `0` or `RTC_ERR_ARG` for NULL or a time `dt_valid()` rejects.

```c
int rtc_detect(const uint8_t regs[RTC_PROBE_LEN]);
```

The detection rule above, pure and tested on the host. `regs` holds `RTC_PROBE_LEN` bytes read from register `0x00`. Returns `RTC_CHIP_DS3231`, `RTC_CHIP_DS1307`, or `RTC_CHIP_NONE` for NULL or an all `0x00` or all `0xFF` dump.

[`examples/rtc/main.c`](../../../examples/rtc/main.c) scans the bus, checks the error codes, reads, sets and reads back the time, and uses the DS1307 RAM. Real DS1307, DS1338 and DS3231 chips, and the read-only detection on them, are pending hardware validation. See [hardware validation](../../HARDWARE_VALIDATION.md).

## Date and time (`datetime.h`)

`datetime.h` is pure code in `src/lib/datetime.c`, host-tested. It covers 2000-01-01 00:00:00 to 2099-12-31 23:59:59, the range of a two-digit RTC year. It has no time zones and no floating point.

```c
typedef struct {
    uint16_t year;      /* 2000 to 2099 */
    uint8_t month;      /* 1 to 12 */
    uint8_t day;        /* 1 to 28, 29, 30 or 31 */
    uint8_t hour;       /* 0 to 23 */
    uint8_t minute;     /* 0 to 59 */
    uint8_t second;     /* 0 to 59 */
    uint8_t weekday;    /* 0 Sunday to 6 Saturday */
} datetime_t;
```

| Constant | Value | Meaning |
|---|---|---|
| `DT_YEAR_MIN`, `DT_YEAR_MAX` | `2000`, `2099` | Accepted years |
| `DT_SECONDS_MAX` | `3155759999` | Seconds from 2000-01-01 00:00:00 to 2099-12-31 23:59:59 |
| `DT_ISO_SIZE` | `20` | Buffer size for `dt_format_iso()` |
| `DT_ERR_ARG` | `-1` | NULL pointer or buffer too small |
| `DT_ERR_RANGE` | `-2` | Field or value out of range |

```c
int dt_is_leap(unsigned year);
unsigned dt_days_in_month(unsigned year, unsigned month);
int dt_weekday(unsigned year, unsigned month, unsigned day);
```

`dt_is_leap()` returns `1` or `0`, Gregorian rule, any year. `dt_days_in_month()` returns `0` for a month outside 1 to 12. `dt_weekday()` returns 0 to 6, or `-1` for an invalid date or one outside 2000 to 2099.

```c
int dt_valid(const datetime_t *dt);
```

Returns `0` when every field except `weekday` is in range, `DT_ERR_RANGE` when one is not, or `DT_ERR_ARG` for NULL. The weekday is not checked, because the other fields fix it.

```c
int dt_to_seconds(const datetime_t *dt, uint32_t *out);
int dt_from_seconds(uint32_t seconds, datetime_t *out);
```

Converts to and from seconds since 2000-01-01 00:00:00. `dt_to_seconds()` returns the `dt_valid()` result for a bad `dt`. `dt_from_seconds()` fills in the weekday and returns `DT_ERR_RANGE` above `DT_SECONDS_MAX`.

```c
int dt_to_fat(const datetime_t *dt, uint16_t *fat_date, uint16_t *fat_time);
int dt_from_fat(uint16_t fat_date, uint16_t fat_time, datetime_t *out);
```

FAT directory stamps.

| Stamp | Bits |
|---|---|
| Date | 15 to 9 year minus 1980, 8 to 5 month, 4 to 0 day |
| Time | 15 to 11 hour, 10 to 5 minute, 4 to 0 second divided by 2 |

Odd seconds round down. `dt_from_fat()` returns `DT_ERR_RANGE` for a year outside 2000 to 2099 (FAT stamps reach 2107) or an invalid field, and fills in the weekday.

```c
uint8_t dt_bcd_encode(unsigned value);
int dt_bcd_decode(uint8_t bcd);
```

Two BCD digits. `dt_bcd_encode()` returns `0xff` above 99. `dt_bcd_decode()` returns `-1` when a digit is above 9.

```c
int dt_format_iso(const datetime_t *dt, char *buf, size_t size);
```

Writes `YYYY-MM-DDTHH:MM:SS` and a NUL, with no zone suffix. `size` must be at least `DT_ISO_SIZE`. Returns `19`, or a negative error. On error `buf` holds `""` when `size` is not 0.

All functions are reentrant and safe in any context.

```c
datetime_t dt;
uint16_t d, t;

dt_from_seconds(0, &dt);         /* 2000-01-01T00:00:00, Saturday */
dt_to_fat(&dt, &d, &t);          /* d = 0x2821, t = 0 */
```

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

Muxes the five pins, reads the core clock (mailbox clock id 4, else `SPI_CORE_FALLBACK_HZ`) and sets both chip selects to `SPI_DEFAULT_HZ`, mode 0, active low. Returns `0`, or `SPI_ERR_INIT` when the board is unknown or a pin cannot be muxed. It also ends any open transaction.

```c
int spi_configure(unsigned cs, uint32_t hz, unsigned mode);
uint32_t spi_get_hz(unsigned cs);
```

Sets the rate and mode of one chip select. The divider is `core / hz` rounded up, then rounded up to an even value, at least 2. So the clock never runs faster than `hz`, and the fastest rate is core / 2. The slowest is core / 65534, about 3.8 kHz at 250 MHz. A rate of 0, or one that needs a larger divider, gives `SPI_ERR_ARG`. The new settings apply at the next `spi_begin()` or transfer. `spi_configure()` returns `SPI_ERR_BUSY` while any transaction is open, because the chip select polarity bits are shared. `spi_get_hz()` returns the rate after rounding, or `0` before init or for a bad `cs`.

```c
int spi_transfer(unsigned cs, const void *tx, void *rx, size_t len);
int spi_write(unsigned cs, const void *tx, size_t len);
```

`spi_transfer()` clocks `len` bytes full duplex. With `tx` NULL it sends zeros. With `rx` NULL it drops what comes in. Chip select stays asserted for the whole call. `len` 0 returns `0` and does nothing. `spi_write()` is `spi_transfer()` with `rx` NULL. Its `tx` must not be NULL when `len` is not 0. Each call has a deadline of 20 ms plus four times the wire time of `(len + 1) * 8` bits. A timeout clears the FIFOs and releases chip select, also inside a `spi_begin()` pair.

```c
int spi_begin(unsigned cs);
int spi_end(unsigned cs);
```

`spi_begin()` asserts `cs` and keeps it asserted across later transfers on the same `cs`, until `spi_end()`. Between them you can change GPIOs, for example a display's data/command pin. A call for the other chip select returns `SPI_ERR_BUSY`. `spi_begin()` on the open `cs` returns `0`. `spi_end()` waits for the last byte, then releases chip select. With no open transaction it returns `0`. On the other chip select it returns `SPI_ERR_BUSY`.

Context: main context only. No lock.

| Sharp edge | Detail |
|---|---|
| Core clock | The rate depends on the core clock read at init, as for I2C. |
| FIFO gaps | The driver keeps at most 16 bytes in flight, so it never overruns the RX FIFO. Long transfers run at the bus rate, with a short gap every 16 bytes. |
| QEMU | The model ignores the rate, the mode and chip select, and shifts in zeros when no device is attached. An attached `w25q32` flash answers only the first command, because it never sees chip select change. |

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

[`examples/spi/main.c`](../../../examples/spi/main.c) reads a JEDEC ID, checks every mode on both chip selects, and runs the same begin, write, GPIO, write, end pattern.

Wiring: MOSI on header pin 19, MISO on pin 21, SCLK on pin 23, CE0 on pin 24, CE1 on pin 26. All lines are 3.3 V.

Clock rate, modes on the wire, chip select levels and the timeout path are pending hardware validation. See [hardware validation](../../HARDWARE_VALIDATION.md).

## Configuration files (`ini.h`)

`ini.h` is a strict INI parser for text already in memory, such as a file read from the SD card. It is pure code in `src/lib/ini.c`, host-tested. It does not allocate and keeps no global state, so it is reentrant. It does not write INI files.

Syntax, one item per line:

| Line | Meaning |
|---|---|
| `[name]` | Starts a section. A comment may follow. |
| `key = value` | A key in the current section. Keys before any section belong to the section `""`. |
| `# text` or `; text` | Comment |
| Empty, or spaces and tabs only | Ignored |

| Rule | Detail |
|---|---|
| Names | Section names and keys use `A` to `Z`, `a` to `z`, `0` to `9`, `_`, `.` and `-`. They are case-sensitive. An empty name is a syntax error. A section name may also hold spaces and tabs, as in `[entry UOS]`. Leading and trailing ones are dropped, inner ones are kept as they are and count toward `INI_MAX_SECTION`. The first `]` ends the name. |
| Line ends | LF or CR LF. The last line may lack a line end. |
| Spaces | Spaces and tabs around names and values are dropped. An empty value is allowed. |
| Trailing comments | In an unquoted value, a `#` or `;` starts a comment when it is the first character of the value or follows a space or tab. So `a = b#c` gives `b#c`, `a = b #c` gives `b`, and `color = #f00` gives an empty value. Quote a value that must start with `#` or contain ` #`. |
| Quotes | A value in double quotes keeps everything inside, with the escapes `\"`, `\\` and `\n`. Any other escape, or a missing closing quote, is a syntax error. Only spaces, tabs and a comment may follow the closing quote. |
| Bytes | NUL, a CR that is not the last byte before LF or the end of the text, DEL and control bytes other than tab are errors. Bytes `0x80` and above pass through, so UTF-8 works in values. |

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
| `INI_ERR_ARG` | `-1` | NULL text with a non-zero length, no handler, or a NULL or empty helper argument |
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

Calls `fn` once per key, in file order, with NUL-terminated copies that live until `fn` returns. `line` counts from 1. Return `0` from `fn` to go on, anything else to stop. `ini_parse()` returns `0` at the end, `INI_STOPPED` when `fn` stopped it, or an error. Empty text returns `0`. `err_line`, if not NULL, gets the line of the error, or `0`. Keys before the bad line have already reached `fn`, so check the return code before you use them. The scratch buffers, 192 bytes, live on the stack.

```c
typedef int (*ini_section_handler_t)(const char *section, unsigned line, void *ctx);
int ini_parse_sections(const char *text, size_t len, ini_handler_t fn,
                       ini_section_handler_t on_section, void *ctx, unsigned *err_line);
```

`ini_parse()` with a second handler. `on_section` gets each section header line in file order, before the keys under it reach `fn`. It sees a section with no keys, and each repeat of a name. `section` is a NUL-terminated copy that lives until the handler returns. A non-zero return stops the parse with `INI_STOPPED`, as for `fn`. Either handler may be NULL. With both NULL the call returns `INI_ERR_ARG`. A loader can list the `[entry NAME]` sections of `BOOT.CFG` with it, empty ones included. `ini_parse()` calls it with `on_section` NULL, so existing callers see no change.

```c
int ini_get(const char *text, size_t len, const char *section, const char *key,
            char *out, size_t out_size);
```

Copies the value of `key` in `section` into `out`. Use `""` for keys before any section. When a key appears twice in a section, the last one wins. The callback sees both. The whole text must parse, so a syntax error anywhere fails the lookup. Each call parses the whole text again. On any error `out` holds `""`.

```c
int ini_get_int(const char *text, size_t len, const char *section, const char *key,
                int32_t min, int32_t max, int32_t *out);
int ini_get_bool(const char *text, size_t len, const char *section, const char *key,
                 int *out);
```

`ini_get_int()` accepts decimal with an optional `+` or `-`, or `0x` or `0X` hex without a sign. It returns `INI_ERR_VALUE` for anything else and `INI_ERR_RANGE` unless `min <= value <= max`. `ini_get_bool()` accepts `true`/`false`, `yes`/`no`, `on`/`off` and `1`/`0`, in any case, and stores `1` or `0`. `*out` changes only on success.

```c
const char *ini_strerror(int code);
```

Short text for `0`, `INI_STOPPED` or an `INI_ERR_*` code, or `"unknown error"`.

All functions are reentrant and safe in any context, but a large file takes time to parse. Keep it out of IRQ context.

```c
static const char conf[] = "[display]\nwidth = 640\nscanlines = on\n";
int32_t width;
int scan;

if (ini_get_int(conf, sizeof(conf) - 1, "display", "width", 320, 1920, &width) == 0
    && ini_get_bool(conf, sizeof(conf) - 1, "display", "scanlines", &scan) == 0) {
    k_printf("width=%d scanlines=%d\n", (int)width, scan);
}
```
