# SPI display

This page covers the ILI9341 TFT panel driver. Return codes and context rules follow the [Conventions](../API.md#conventions). Nothing on this page is validated on hardware yet.

## TFT panel (`tft.h`)

`tft.h` drives an ILI9341 panel on SPI0 through `spi.h` and `gpio.h`. The glass is 240x320. Rotation 90 and 270 use it as 320x240. Pixels are RGB565, 16 bits each, sent high byte first. The driver polls, runs in the main context only and allocates nothing.

### Wiring

| Panel pin | Pi pin | Notes |
|---|---|---|
| SCK | GPIO 11, SPI0 SCLK | |
| SDI (MOSI) | GPIO 10, SPI0 MOSI | |
| SDO (MISO) | GPIO 9, SPI0 MISO | Only `tft_read_id()` uses it |
| CS | GPIO 8 (CE0) or GPIO 7 (CE1) | `cs` in the config |
| D/C | Any free GPIO, default GPIO 25 | Low for a command byte, high for data |
| RESET | Any free GPIO, default GPIO 4, or none | Active low. Without it the driver sends a software reset. |
| LED | 3.3 V, or a GPIO | A GPIO only switches it on and off. A panel LED pin may draw more than a GPIO can give: check the module. |

[BOARD_SUPPORT.md](../../spec/BOARD_SUPPORT.md#spi-display) explains the default pins.

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `TFT_ERR_ARG` | `-1` | Bad config field, rotation, source order or NULL buffer |
| `TFT_ERR_INIT` | `-2` | `tft_init()` has not run or failed |
| `TFT_ERR_BOUNDS` | `-3` | The rectangle is empty or not inside the panel |
| `TFT_ERR_BUS` | `-4` | An SPI or GPIO call failed |
| `TFT_ERR_TIMER` | `-5` | The system timer does not run |
| `TFT_ERR_NO_PANEL` | `-6` | The id read gave all zeros or all ones |
| `TFT_PIN_NONE` | `-1` | No reset or backlight pin |
| `TFT_DEFAULT_HZ` | `32000000` | Default SPI clock asked for |
| `TFT_HZ_MIN`, `TFT_HZ_MAX` | `1000000`, `64000000` | Accepted range of `hz` |
| `TFT_READ_HZ` | `4000000` | SPI clock for `tft_read_id()` |
| `TFT_CHUNK_PIXELS` | `64` | Pixels converted per SPI write, in a 128-byte stack buffer |
| `TFT_SRC_XRGB` | `0` | 32-bit source pixel `0x00RRGGBB`, the framebuffer's order |
| `TFT_SRC_XBGR` | `1` | 32-bit source pixel `0x00BBGGRR` |

### Configuration

```c
typedef struct {
    unsigned cs;
    uint32_t hz;
    int dc_pin;
    int reset_pin;
    int backlight_pin;
    unsigned rotation;
    int bgr;
    int invert;
} tft_config_t;
```

| Field | Rule |
|---|---|
| `cs` | `0` or `1` |
| `hz` | `TFT_HZ_MIN` to `TFT_HZ_MAX`. The SPI divider rounds down the rate, so the bus never runs faster. |
| `dc_pin` | GPIO 0 to 53, not GPIO 7 to 11 |
| `reset_pin` | Same rule, or `TFT_PIN_NONE`. Differs from `dc_pin`. |
| `backlight_pin` | Same rule, or `TFT_PIN_NONE`. Differs from the two pins above. High is on. |
| `rotation` | `0`, `90`, `180` or `270` |
| `bgr` | Non-zero sets the BGR bit of MADCTL, for panels wired blue first |
| `invert` | Non-zero sends INVON, zero sends INVOFF |

`TFT_CONFIG_DEFAULT` is `{ 0, TFT_DEFAULT_HZ, 25, 4, TFT_PIN_NONE, 90, 1, 0 }`: CE0, DC on GPIO 25, reset on GPIO 4, no backlight pin, landscape, BGR, no inversion.

### Init

```c
int tft_init(const tft_config_t *cfg);
unsigned tft_width(void);
unsigned tft_height(void);
uint32_t tft_get_hz(void);
```

`tft_init()` checks every field and returns `TFT_ERR_ARG` before it touches a pin. It then reads the system timer a bounded number of times. If the counter does not move, it returns `TFT_ERR_TIMER` instead of waiting forever, as `audio_init()` does. It runs `spi_init()` only when SPI0 has no rate yet, so an SPI setup made before is kept. It sets the rate with `spi_configure()` in mode 0 and drives DC high and the backlight off.

Then it resets the panel and sends a short sequence. Each wait comes from the ILI9341 data sheet.

| Step | Bytes | Wait after |
|---|---|---|
| Reset pin low, then high | | 10 us low, the minimum reset pulse. Then 120 ms: commands need 5 ms, SLPOUT needs 120 ms. |
| Or, without a reset pin: SWRESET | `01` | 120 ms, for the same reason |
| SLPOUT | `11` | 120 ms. The next command needs 5 ms, and SLPIN may only follow after 120 ms. |
| COLMOD, 16 bits per pixel | `3a 55` | |
| MADCTL, rotation and colour order | `36 xx` | |
| INVON or INVOFF | `21` or `20` | |
| DISPON | `29` | |
| Backlight pin high | | |

| Rotation | MADCTL bits | Size |
|---|---|---|
| 0 | MX | 240x320 |
| 90 | MV | 320x240 |
| 180 | MY | 240x320 |
| 270 | MY, MX, MV | 320x240 |

The BGR bit, `0x08`, is added when `bgr` is set. These values assume the glass is mounted the way common 2.4" and 2.8" modules mount it. On another module the picture may come out mirrored: then change the rotation or the module.

Power control, VCOM, frame rate and gamma stay at the values the panel loads at reset. A panel that comes up dim or with odd colours may need them. This driver does not guess them.

`tft_width()` and `tft_height()` give the size for the rotation, or `0` before init. `tft_get_hz()` gives the SPI rate actually set, `core / divider`, or `0` before init.

### Drawing

```c
int tft_write_rect(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *pixels);
int tft_fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t colour);
```

Each call sends CASET and PASET with inclusive end coordinates, high byte first, then RAMWR and the pixels. `pixels` holds `w * h` values, row by row, with no padding. A rectangle that is empty or not fully inside the panel returns `TFT_ERR_BOUNDS` and sends nothing. The driver never clips.

```c
int tft_present_px32(const uint32_t *src, uint32_t pitch_bytes, unsigned order);
int tft_present_px32_rect(const uint32_t *src, uint32_t pitch_bytes, unsigned order,
                          unsigned x, unsigned y, unsigned w, unsigned h);
```

`tft_present_px32()` sends a whole frame from a 32-bit buffer of `tft_width()` by `tft_height()` pixels, such as the one Pip-OS draws into. Rows are `pitch_bytes` apart, a multiple of 4 and at least 4 times the width. `order` is `TFT_SRC_XRGB` or `TFT_SRC_XBGR`. `tft_present_px32_rect()` sends only one rectangle of the same buffer: `src` still points at pixel (0, 0). Both convert to RGB565 on the fly, `TFT_CHUNK_PIXELS` at a time, and never write to `src`.

### Power and id

```c
int tft_sleep(int on);
int tft_display(int on);
int tft_read_id(uint32_t *id);
```

`tft_sleep(1)` sends SLPIN and `tft_sleep(0)` sends SLPOUT. Both wait 120 ms. `tft_display()` sends DISPON or DISPOFF. The panel keeps its memory in both states, and pixels sent meanwhile still land in it. The backlight pin, when set, is on only while the display is on and the panel is awake.

A failed call leaves the driver state as it was after the last call that succeeded, and the backlight goes back to match it. The caller can retry the same call. A failed `tft_init()` leaves the driver uninitialised, so other calls return `TFT_ERR_INIT` until an init succeeds. `tft_sleep()` returns `TFT_ERR_TIMER` and sends nothing when the system timer has stopped.

`tft_read_id()` sends RDDID (`04`) at `TFT_READ_HZ` and stores the next four bytes, the first in the top bits. The data sheet's read cycle is slower than its write cycle, hence the lower rate. The data sheet puts a dummy clock before the three id bytes. Whether that is one bit or a whole byte on the 4-wire interface is not certain, so the driver returns the raw bytes and does not compare them with an expected id. All zeros or all ones, which an absent panel or an unwired MISO gives, return `TFT_ERR_NO_PANEL`.

### Tap and pixel helpers

```c
typedef void (*tft_tap_fn)(int dc, const uint8_t *bytes, size_t len);
void tft_set_tap(tft_tap_fn fn);
uint16_t tft_rgb565(unsigned r, unsigned g, unsigned b);
uint16_t tft_px32_to_565(uint32_t px, unsigned order);
```

`tft_set_tap()` registers a function that sees every byte written to the panel with its DC level. `examples/tft` uses it to checksum the bus. `NULL` turns it off.

`tft_rgb565()` keeps the top 5 bits of red and blue and the top 6 bits of green. It truncates and does not round. `tft_px32_to_565()` does the same for a 32-bit pixel in either order. Both are pure and run on the host.

### Time per frame

A full frame is 320 x 240 x 2 = 153600 pixel bytes, plus 11 bytes of CASET, PASET and RAMWR. At exactly 32 MHz that is 153611 x 8 / 32000000 = 38.4 ms. The real rate is the core clock over an even divider: with a 250 MHz core clock, 32 MHz asked gives 31.25 MHz and 39.3 ms. The SPI driver also stops the clock briefly every 16 bytes and between writes. That overhead is not measured yet. A dirty rectangle costs 2 bytes per pixel plus the same 11 bytes.

### Not done

- Touch controllers.
- Backlight dimming. The pin is on or off.
- Tearing sync. The TE pin is not read, so a frame can show half old, half new.
- Other controllers, such as the ST7789 or the ILI9488.
- DMA. Every byte goes through the polled SPI driver.

### Tests

`tests/host/test_tft_bus.c` checks exact transcripts on a fake bus: init for each rotation and colour order, config checks, windows, byte order, chunk sizes and bounds. `tests/host/test_tft_panel.c` plays the transcripts into a panel model written from the data sheet and compares the picture. `tests/target/test_tft.py` boots `examples/tft` on the four QEMU boards. QEMU has no panel model, so it compares a CRC of the bytes sent with the one `build/host/tft_tool` computes from the same calls.
