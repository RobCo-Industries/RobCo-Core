/*
 * ILI9341 over SPI0. Command names and waits follow the ILI9341 data
 * sheet (Ilitek, V1.11). Not validated on hardware.
 *
 * The init sequence is minimal: sleep out, 16-bit pixels, memory access
 * control, inversion, display on. Power control, VCOM, frame rate and
 * gamma stay at the reset defaults the panel loads itself.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/gpio.h>
#include <robco/spi.h>
#include <robco/tft.h>
#include <robco/timer.h>

/* Commands, data sheet chapter 8. */
#define CMD_SWRESET     0x01u
#define CMD_RDDID       0x04u
#define CMD_SLPIN       0x10u
#define CMD_SLPOUT      0x11u
#define CMD_INVOFF      0x20u
#define CMD_INVON       0x21u
#define CMD_DISPOFF     0x28u
#define CMD_DISPON      0x29u
#define CMD_CASET       0x2au
#define CMD_PASET       0x2bu
#define CMD_RAMWR       0x2cu
#define CMD_MADCTL      0x36u
#define CMD_COLMOD      0x3au

/* COLMOD: 16 bits per pixel on both the RGB and the MCU interface. */
#define COLMOD_16BIT    0x55u

/* MADCTL bits: row order (MY), column order (MX), row/column exchange (MV), BGR. */
#define MADCTL_MY       0x80u
#define MADCTL_MX       0x40u
#define MADCTL_MV       0x20u
#define MADCTL_BGR      0x08u

#define GLASS_W         240u
#define GLASS_H         320u

/*
 * Waits. Reset low pulse: at least 10 us (tRW). After a reset, hardware
 * or SWRESET: 5 ms before any command and 120 ms before SLPOUT (tRT), so
 * 120 ms. After SLPOUT: 5 ms before the next command, but 120 ms before
 * an SLPIN may follow, so 120 ms. After SLPIN: 5 ms, and 120 ms before an
 * SLPOUT, so 120 ms again. tft_sleep() can then be called back to back.
 */
#define RESET_PULSE_US  10u
#define RESET_WAIT_MS   120u
#define SLEEP_WAIT_MS   120u

/* Equal timer reads in a row before the timer counts as stopped, as in audio_init(). */
#define TIMER_STALL_POLLS 1000000u

static tft_config_t cfg;
static int ready;
static unsigned width, height;
static uint32_t rate_hz;
static tft_tap_fn tap;
static int asleep;
static int shown;

/* A pixel source for stream(): one colour, RGB565 rows or 32-bit rows. */
typedef struct {
    const uint16_t *p16;    /* w pixels per row */
    const uint32_t *p32;    /* full frame, pitch words per row */
    uint32_t pitch;         /* in 32-bit words */
    unsigned order;
    uint16_t colour;
} source_t;

static int pin_ok(int pin)
{
    /* GPIO 7 to 11 belong to SPI0. */
    return pin >= 0 && pin < GPIO_PIN_COUNT && (pin < 7 || pin > 11);
}

static int check_config(const tft_config_t *c)
{
    if (c == NULL || c->cs >= SPI_CS_COUNT) {
        return TFT_ERR_ARG;
    }
    if (c->hz < TFT_HZ_MIN || c->hz > TFT_HZ_MAX) {
        return TFT_ERR_ARG;
    }
    if (c->rotation != 0 && c->rotation != 90 && c->rotation != 180 && c->rotation != 270) {
        return TFT_ERR_ARG;
    }
    if (!pin_ok(c->dc_pin)) {
        return TFT_ERR_ARG;
    }
    if (c->reset_pin != TFT_PIN_NONE &&
        (!pin_ok(c->reset_pin) || c->reset_pin == c->dc_pin)) {
        return TFT_ERR_ARG;
    }
    if (c->backlight_pin != TFT_PIN_NONE &&
        (!pin_ok(c->backlight_pin) || c->backlight_pin == c->dc_pin ||
         c->backlight_pin == c->reset_pin)) {
        return TFT_ERR_ARG;
    }
    return 0;
}

/* Non-zero when the system timer advances within a bounded number of reads. */
static int timer_runs(void)
{
    uint32_t t0 = timer_now_lo();
    uint32_t i;

    for (i = 0; i < TIMER_STALL_POLLS; i++) {
        if (timer_now_lo() != t0) {
            return 1;
        }
    }
    return 0;
}

static uint8_t madctl(const tft_config_t *c)
{
    static const uint8_t rot[4] = {
        MADCTL_MX,                              /* 0: portrait */
        MADCTL_MV,                              /* 90: landscape */
        MADCTL_MY,                              /* 180: portrait, upside down */
        MADCTL_MX | MADCTL_MY | MADCTL_MV,      /* 270: landscape, upside down */
    };

    return (uint8_t)(rot[c->rotation / 90u] | (c->bgr ? MADCTL_BGR : 0u));
}

/* Writes bytes inside an open transaction with DC at `dc`. */
static int send(int dc, const uint8_t *p, size_t n)
{
    if (gpio_write((unsigned)cfg.dc_pin, dc) < 0) {
        return TFT_ERR_BUS;
    }
    if (spi_write(cfg.cs, p, n) < 0) {
        return TFT_ERR_BUS;
    }
    if (tap != NULL) {
        tap(dc, p, n);
    }
    return 0;
}

/* Ends the transaction; keeps the first error. */
static int finish(int rc)
{
    int e = spi_end(cfg.cs);

    if (rc == 0 && e < 0) {
        rc = TFT_ERR_BUS;
    }
    return rc;
}

/* One command and its parameters in one chip select transaction. */
static int command(uint8_t cmd, const uint8_t *params, size_t n)
{
    int rc;

    if (spi_begin(cfg.cs) < 0) {
        return TFT_ERR_BUS;
    }
    rc = send(0, &cmd, 1);
    if (rc == 0 && n != 0) {
        rc = send(1, params, n);
    }
    return finish(rc);
}

static int backlight(int on)
{
    if (cfg.backlight_pin == TFT_PIN_NONE) {
        return 0;
    }
    return gpio_write((unsigned)cfg.backlight_pin, on) < 0 ? TFT_ERR_BUS : 0;
}

/*
 * asleep and shown follow the last panel command that succeeded. A failed
 * panel command leaves them as they were. The backlight is derived from
 * them: on only when shown and awake. A failed backlight write returns an
 * error but does not change the panel state; the next tft_sleep() or
 * tft_display() call writes the backlight again.
 */
static int light_follows(void)
{
    return backlight(shown && !asleep);
}

int tft_init(const tft_config_t *c)
{
    uint8_t p;
    int rc;

    ready = 0;
    width = height = 0;
    rate_hz = 0;
    rc = check_config(c);
    if (rc < 0) {
        return rc;
    }
    if (!timer_runs()) {
        return TFT_ERR_TIMER;
    }
    cfg = *c;
    if (spi_get_hz(cfg.cs) == 0 && spi_init() < 0) {
        return TFT_ERR_BUS;
    }
    if (spi_configure(cfg.cs, cfg.hz, SPI_MODE_0) < 0) {
        return TFT_ERR_BUS;
    }
    if (gpio_config_output((unsigned)cfg.dc_pin, 1) < 0) {
        return TFT_ERR_BUS;
    }
    if (cfg.backlight_pin != TFT_PIN_NONE &&
        gpio_config_output((unsigned)cfg.backlight_pin, 0) < 0) {
        return TFT_ERR_BUS;
    }
    if (cfg.reset_pin != TFT_PIN_NONE) {
        if (gpio_config_output((unsigned)cfg.reset_pin, 0) < 0) {
            return TFT_ERR_BUS;
        }
        timer_delay_us(RESET_PULSE_US);
        if (gpio_write((unsigned)cfg.reset_pin, 1) < 0) {
            return TFT_ERR_BUS;
        }
    } else if (command(CMD_SWRESET, NULL, 0) < 0) {
        return TFT_ERR_BUS;
    }
    timer_delay_ms(RESET_WAIT_MS);

    if (command(CMD_SLPOUT, NULL, 0) < 0) {
        return TFT_ERR_BUS;
    }
    timer_delay_ms(SLEEP_WAIT_MS);
    p = COLMOD_16BIT;
    if (command(CMD_COLMOD, &p, 1) < 0) {
        return TFT_ERR_BUS;
    }
    p = madctl(&cfg);
    if (command(CMD_MADCTL, &p, 1) < 0) {
        return TFT_ERR_BUS;
    }
    if (command(cfg.invert ? CMD_INVON : CMD_INVOFF, NULL, 0) < 0) {
        return TFT_ERR_BUS;
    }
    if (command(CMD_DISPON, NULL, 0) < 0) {
        return TFT_ERR_BUS;
    }
    if (backlight(1) < 0) {
        return TFT_ERR_BUS;
    }
    asleep = 0;
    shown = 1;
    if (cfg.rotation == 90 || cfg.rotation == 270) {
        width = GLASS_H;
        height = GLASS_W;
    } else {
        width = GLASS_W;
        height = GLASS_H;
    }
    rate_hz = spi_get_hz(cfg.cs);
    ready = 1;
    return 0;
}

unsigned tft_width(void)
{
    return width;
}

unsigned tft_height(void)
{
    return height;
}

uint32_t tft_get_hz(void)
{
    return rate_hz;
}

void tft_set_tap(tft_tap_fn fn)
{
    tap = fn;
}

static int check_rect(unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (!ready) {
        return TFT_ERR_INIT;
    }
    if (w == 0 || h == 0 || x >= width || y >= height || w > width - x || h > height - y) {
        return TFT_ERR_BOUNDS;
    }
    return 0;
}

static void put16(uint8_t *b, unsigned v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static uint16_t pixel(const source_t *s, unsigned x, unsigned y, unsigned col, unsigned row,
                      unsigned w)
{
    if (s->p16 != NULL) {
        return s->p16[row * w + col];
    }
    if (s->p32 != NULL) {
        return tft_px32_to_565(s->p32[(y + row) * s->pitch + x + col], s->order);
    }
    return s->colour;
}

/*
 * Sets the window (inclusive end coordinates, big-endian), sends RAMWR,
 * then the pixels high byte first, TFT_CHUNK_PIXELS at a time.
 */
static int stream(unsigned x, unsigned y, unsigned w, unsigned h, const source_t *s)
{
    uint8_t buf[TFT_CHUNK_PIXELS * 2u];
    uint8_t win[4];
    uint8_t cmd = CMD_RAMWR;
    unsigned col = 0;
    unsigned row = 0;
    unsigned n;
    int rc;

    put16(win, x);
    put16(win + 2, x + w - 1u);
    rc = command(CMD_CASET, win, 4);
    if (rc < 0) {
        return rc;
    }
    put16(win, y);
    put16(win + 2, y + h - 1u);
    rc = command(CMD_PASET, win, 4);
    if (rc < 0) {
        return rc;
    }
    if (spi_begin(cfg.cs) < 0) {
        return TFT_ERR_BUS;
    }
    rc = send(0, &cmd, 1);
    while (rc == 0 && row < h) {
        for (n = 0; n < TFT_CHUNK_PIXELS && row < h; n++) {
            put16(buf + 2u * n, pixel(s, x, y, col, row, w));
            if (++col == w) {
                col = 0;
                row++;
            }
        }
        rc = send(1, buf, 2u * n);
    }
    return finish(rc);
}

int tft_write_rect(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *pixels)
{
    source_t s = { 0 };
    int rc = check_rect(x, y, w, h);

    if (rc < 0) {
        return rc;
    }
    if (pixels == NULL) {
        return TFT_ERR_ARG;
    }
    s.p16 = pixels;
    return stream(x, y, w, h, &s);
}

int tft_fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t colour)
{
    source_t s = { 0 };
    int rc = check_rect(x, y, w, h);

    if (rc < 0) {
        return rc;
    }
    s.colour = colour;
    return stream(x, y, w, h, &s);
}

int tft_present_px32_rect(const uint32_t *src, uint32_t pitch_bytes, unsigned order,
                          unsigned x, unsigned y, unsigned w, unsigned h)
{
    source_t s = { 0 };
    int rc = check_rect(x, y, w, h);

    if (rc < 0) {
        return rc;
    }
    if (src == NULL || (pitch_bytes & 3u) != 0 || pitch_bytes / 4u < width ||
        order > TFT_SRC_XBGR) {
        return TFT_ERR_ARG;
    }
    s.p32 = src;
    s.pitch = pitch_bytes / 4u;
    s.order = order;
    return stream(x, y, w, h, &s);
}

int tft_present_px32(const uint32_t *src, uint32_t pitch_bytes, unsigned order)
{
    if (!ready) {
        return TFT_ERR_INIT;
    }
    return tft_present_px32_rect(src, pitch_bytes, order, 0, 0, width, height);
}

int tft_sleep(int on)
{
    int rc;

    if (!ready) {
        return TFT_ERR_INIT;
    }
    if (!timer_runs()) {
        return TFT_ERR_TIMER;
    }
    on = on != 0;
    if (on) {
        rc = backlight(0);
        if (rc == 0) {
            rc = command(CMD_SLPIN, NULL, 0);
        }
        timer_delay_ms(SLEEP_WAIT_MS);
        if (rc == 0) {
            asleep = 1;
        } else {
            (void)light_follows();
        }
    } else {
        rc = command(CMD_SLPOUT, NULL, 0);
        timer_delay_ms(SLEEP_WAIT_MS);
        if (rc == 0) {
            /* The panel is awake: record it before the backlight write. */
            asleep = 0;
            rc = light_follows();
        } else {
            (void)light_follows();
        }
    }
    return rc;
}

int tft_display(int on)
{
    int rc;

    if (!ready) {
        return TFT_ERR_INIT;
    }
    on = on != 0;
    if (on) {
        rc = command(CMD_DISPON, NULL, 0);
        if (rc == 0) {
            /* The panel shows its memory: record it before the backlight write. */
            shown = 1;
            rc = light_follows();
        } else {
            (void)light_follows();
        }
    } else {
        rc = backlight(0);
        if (rc == 0) {
            rc = command(CMD_DISPOFF, NULL, 0);
        }
        if (rc == 0) {
            shown = 0;
        } else {
            (void)light_follows();
        }
    }
    return rc;
}

/*
 * The data sheet says RDDID answers a dummy clock, then three id bytes.
 * Whether the dummy is one bit or a whole byte on the 4-wire serial
 * interface is not certain, so the four bytes are returned raw.
 */
int tft_read_id(uint32_t *id)
{
    uint8_t cmd = CMD_RDDID;
    uint8_t rx[4] = { 0, 0, 0, 0 };
    uint32_t v;
    int rc;

    if (!ready) {
        return TFT_ERR_INIT;
    }
    if (id == NULL) {
        return TFT_ERR_ARG;
    }
    if (spi_configure(cfg.cs, TFT_READ_HZ, SPI_MODE_0) < 0) {
        return TFT_ERR_BUS;
    }
    if (spi_begin(cfg.cs) < 0) {
        rc = TFT_ERR_BUS;
    } else {
        rc = send(0, &cmd, 1);
        if (rc == 0 && (gpio_write((unsigned)cfg.dc_pin, 1) < 0 ||
                        spi_transfer(cfg.cs, NULL, rx, sizeof(rx)) < 0)) {
            rc = TFT_ERR_BUS;
        }
        rc = finish(rc);
    }
    if (spi_configure(cfg.cs, cfg.hz, SPI_MODE_0) < 0 && rc == 0) {
        rc = TFT_ERR_BUS;
    }
    if (rc < 0) {
        return rc;
    }
    v = (uint32_t)rx[0] << 24 | (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
    *id = v;
    if (v == 0 || v == 0xffffffffu) {
        return TFT_ERR_NO_PANEL;
    }
    return 0;
}
