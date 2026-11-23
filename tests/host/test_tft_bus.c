/* ILI9341 driver against a fake bus: exact transcripts, bounds, chunks. */
#include <stdint.h>

#include "rc_test.h"
#include "tft_fake.h"

#define DC      25
#define RST     4
#define BL      5

static uint16_t pix[320 * 240];
static uint32_t frame[320 * 240];

static tft_config_t config(void)
{
    tft_config_t c = TFT_CONFIG_DEFAULT;

    return c;
}

/* Index of the first event that differs from `want`, or -1 when all match. */
static long diff(const ev_t *want, size_t n)
{
    size_t i;

    for (i = 0; i < n && i < fake_n; i++) {
        if (fake_ev[i].kind != want[i].kind || fake_ev[i].a != want[i].a ||
            fake_ev[i].b != want[i].b) {
            printf("  event %zu: got %u/%u/%u, want %u/%u/%u\n", i, fake_ev[i].kind,
                   fake_ev[i].a, (unsigned)fake_ev[i].b, want[i].kind, want[i].a,
                   (unsigned)want[i].b);
            return (long)i;
        }
    }
    if (n != fake_n) {
        printf("  %zu events, want %zu\n", fake_n, n);
        return (long)i;
    }
    return -1;
}

#define EXPECT(arr) CHECK_EQ(diff((arr), sizeof(arr) / sizeof((arr)[0])), -1)

static int init_with(const tft_config_t *c)
{
    fake_reset(c->dc_pin);
    return tft_init(c);
}

/* Default config, then a clean transcript. */
static void up(unsigned rotation)
{
    tft_config_t c = config();

    c.rotation = rotation;
    CHECK_EQ(init_with(&c), 0);
    fake_clear();
}

/* Default wiring: reset on GPIO 4, no backlight, rotation 90, BGR, no inversion. */
TEST(init_transcript_default)
{
    static const ev_t want[] = {
        RATE(32000000),
        PIN(RST, 0), US(10), PIN(RST, 1),   /* hardware reset: 10 us pulse */
        MS(120),                            /* tRT: 120 ms before SLPOUT */
        CSL, C(0x11), CSH,                  /* SLPOUT */
        MS(120),                            /* 120 ms before a later SLPIN */
        CSL, C(0x3a), D(0x55), CSH,         /* COLMOD: 16 bits per pixel */
        CSL, C(0x36), D(0x28), CSH,         /* MADCTL: MV | BGR */
        CSL, C(0x20), CSH,                  /* INVOFF */
        CSL, C(0x29), CSH,                  /* DISPON */
    };
    tft_config_t c = config();

    CHECK_EQ(init_with(&c), 0);
    EXPECT(want);
    CHECK_EQ(fake_spi_init_calls, 1);
    CHECK_EQ(tft_width(), 320);
    CHECK_EQ(tft_height(), 240);
    CHECK_EQ(tft_get_hz(), 31250000);   /* 250 MHz over 8 */
}

/* No reset line: SWRESET instead. A backlight pin: off first, on last. Inversion on. */
TEST(init_transcript_soft_reset_backlight_invert)
{
    static const ev_t want[] = {
        RATE(32000000),
        PIN(BL, 0),
        CSL, C(0x01), CSH,                  /* SWRESET */
        MS(120),
        CSL, C(0x11), CSH,                  /* SLPOUT */
        MS(120),
        CSL, C(0x3a), D(0x55), CSH,         /* COLMOD */
        CSL, C(0x36), D(0x28), CSH,         /* MADCTL */
        CSL, C(0x21), CSH,                  /* INVON */
        CSL, C(0x29), CSH,                  /* DISPON */
        PIN(BL, 1),
    };
    tft_config_t c = config();

    c.reset_pin = TFT_PIN_NONE;
    c.backlight_pin = BL;
    c.invert = 1;
    CHECK_EQ(init_with(&c), 0);
    EXPECT(want);
}

/* MADCTL for each rotation and colour order; everything else as in the default. */
TEST(init_madctl_per_rotation)
{
    static const struct {
        unsigned rot;
        int bgr;
        uint8_t madctl;
        unsigned w, h;
    } t[] = {
        { 0, 0, 0x40, 240, 320 },       /* MX */
        { 0, 1, 0x48, 240, 320 },       /* MX | BGR */
        { 90, 0, 0x20, 320, 240 },      /* MV */
        { 90, 1, 0x28, 320, 240 },      /* MV | BGR */
        { 180, 0, 0x80, 240, 320 },     /* MY */
        { 180, 1, 0x88, 240, 320 },     /* MY | BGR */
        { 270, 0, 0xe0, 320, 240 },     /* MY | MX | MV */
        { 270, 1, 0xe8, 320, 240 },     /* MY | MX | MV | BGR */
    };
    ev_t want[] = {
        RATE(32000000),
        PIN(RST, 0), US(10), PIN(RST, 1), MS(120),
        CSL, C(0x11), CSH, MS(120),
        CSL, C(0x3a), D(0x55), CSH,
        CSL, C(0x36), D(0x00), CSH,
        CSL, C(0x20), CSH,
        CSL, C(0x29), CSH,
    };
    tft_config_t c = config();
    unsigned i;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        c.rotation = t[i].rot;
        c.bgr = t[i].bgr;
        want[15].a = t[i].madctl;
        CHECK_EQ(init_with(&c), 0);
        EXPECT(want);
        CHECK_EQ(tft_width(), t[i].w);
        CHECK_EQ(tft_height(), t[i].h);
    }
}

TEST(config_validation_each_field)
{
    tft_config_t good = config();
    tft_config_t c;
    unsigned i;
    int rc;

    for (i = 0; i < 16; i++) {
        c = good;
        switch (i) {
        case 0: c.cs = 2; break;
        case 1: c.hz = TFT_HZ_MIN - 1u; break;
        case 2: c.hz = TFT_HZ_MAX + 1u; break;
        case 3: c.rotation = 45; break;
        case 4: c.rotation = 360; break;
        case 5: c.dc_pin = -1; break;
        case 6: c.dc_pin = 54; break;
        case 7: c.dc_pin = 10; break;           /* SPI0 MOSI */
        case 8: c.reset_pin = c.dc_pin; break;
        case 9: c.reset_pin = 54; break;
        case 10: c.reset_pin = 8; break;        /* SPI0 CE0 */
        case 11: c.reset_pin = -2; break;
        case 12: c.backlight_pin = c.dc_pin; break;
        case 13: c.backlight_pin = c.reset_pin; break;
        case 14: c.backlight_pin = 11; break;   /* SPI0 SCLK */
        case 15: c.backlight_pin = 54; break;
        }
        fake_reset(good.dc_pin);
        rc = tft_init(&c);
        if (rc != TFT_ERR_ARG) {
            printf("  case %u: rc %d\n", i, rc);
        }
        CHECK_EQ(rc, TFT_ERR_ARG);
        CHECK_EQ(fake_n, 0);
        CHECK_EQ(fake_spi_init_calls, 0);
        CHECK_EQ(tft_width(), 0);
    }
    fake_reset(good.dc_pin);
    CHECK_EQ(tft_init(NULL), TFT_ERR_ARG);
    CHECK_EQ(tft_fill_rect(0, 0, 1, 1, 0), TFT_ERR_INIT);
    c = good;
    c.hz = TFT_HZ_MIN;
    c.reset_pin = TFT_PIN_NONE;
    c.backlight_pin = 6;
    CHECK_EQ(init_with(&c), 0);
    c.hz = TFT_HZ_MAX;
    CHECK_EQ(init_with(&c), 0);
}

TEST(stopped_timer_fails_before_any_pin)
{
    tft_config_t c = config();

    fake_reset(c.dc_pin);
    fake_timer_frozen = 1;
    CHECK_EQ(tft_init(&c), TFT_ERR_TIMER);
    CHECK_EQ(fake_n, 0);
    CHECK_EQ(fake_spi_init_calls, 0);
    CHECK_EQ(tft_write_rect(0, 0, 1, 1, pix), TFT_ERR_INIT);
}

TEST(spi_already_up_is_not_reinitialised)
{
    tft_config_t c = config();

    fake_reset(c.dc_pin);
    spi_init();
    CHECK_EQ(tft_init(&c), 0);
    CHECK_EQ(fake_spi_init_calls, 1);
}

/* Window commands then RAMWR; returns the index of the first pixel byte. */
static size_t check_window(unsigned x0, unsigned x1, unsigned y0, unsigned y1)
{
    ev_t want[] = {
        CSL, C(0x2a), D(x0 >> 8), D(x0 & 0xff), D(x1 >> 8), D(x1 & 0xff), CSH,  /* CASET */
        CSL, C(0x2b), D(y0 >> 8), D(y0 & 0xff), D(y1 >> 8), D(y1 & 0xff), CSH,  /* PASET */
        CSL, C(0x2c),                                                           /* RAMWR */
    };
    size_t n = sizeof(want) / sizeof(want[0]);
    size_t keep = fake_n;
    long d;

    fake_n = fake_n < n ? fake_n : n;
    d = diff(want, n);
    fake_n = keep;
    CHECK_EQ(d, -1);
    return n;
}

/* Checks the pixel bytes are `n` big-endian values of `want`, then CS high. */
static void check_pixels(size_t at, const uint16_t *want, size_t n)
{
    size_t i;
    unsigned bad = 0;

    CHECK_EQ(fake_n, at + 2u * n + 1u);
    if (fake_n != at + 2u * n + 1u) {
        return;
    }
    for (i = 0; i < n; i++) {
        bad += fake_ev[at + 2 * i].kind != EV_DATA ||
               fake_ev[at + 2 * i].a != (want[i] >> 8) ||
               fake_ev[at + 2 * i + 1].kind != EV_DATA ||
               fake_ev[at + 2 * i + 1].a != (want[i] & 0xff);
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(fake_ev[fake_n - 1].kind, EV_CS_HI);
}

static void pattern(size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pix[i] = (uint16_t)(i * 0x9e37u + 0x1234u);
    }
}

TEST(byte_order_on_the_wire)
{
    static const ev_t want[] = {
        CSL, C(0x2a), D(0x00), D(0x05), D(0x00), D(0x05), CSH,
        CSL, C(0x2b), D(0x00), D(0x07), D(0x00), D(0x07), CSH,
        CSL, C(0x2c), D(0xab), D(0xcd), CSH,
    };
    uint16_t p = 0xabcd;

    up(90);
    CHECK_EQ(tft_write_rect(5, 7, 1, 1, &p), 0);
    EXPECT(want);
}

TEST(write_rect_shapes_and_corners)
{
    static const struct {
        unsigned x, y, w, h;
    } t[] = {
        { 0, 0, 1, 1 }, { 319, 0, 1, 1 }, { 0, 239, 1, 1 }, { 319, 239, 1, 1 },
        { 0, 0, 320, 1 }, { 0, 239, 320, 1 },           /* rows */
        { 0, 0, 1, 240 }, { 319, 0, 1, 240 },           /* columns */
        { 0, 0, 320, 240 },                             /* full frame */
        { 300, 220, 20, 20 }, { 255, 1, 65, 2 },        /* the high byte of x is used */
    };
    unsigned i;
    size_t at;

    up(90);
    pattern(320 * 240);
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        fake_clear();
        CHECK_EQ(tft_write_rect(t[i].x, t[i].y, t[i].w, t[i].h, pix), 0);
        at = check_window(t[i].x, t[i].x + t[i].w - 1, t[i].y, t[i].y + t[i].h - 1);
        check_pixels(at, pix, (size_t)t[i].w * t[i].h);
    }
    CHECK_EQ(fake_overflow, 0);
}

TEST(portrait_bounds)
{
    up(0);
    CHECK_EQ(tft_fill_rect(239, 319, 1, 1, 1), 0);
    CHECK_EQ(tft_fill_rect(240, 0, 1, 1, 1), TFT_ERR_BOUNDS);
    CHECK_EQ(tft_fill_rect(0, 0, 241, 1, 1), TFT_ERR_BOUNDS);
    CHECK_EQ(tft_fill_rect(0, 0, 240, 320, 1), 0);
}

TEST(out_of_bounds_sends_nothing)
{
    static const struct {
        unsigned x, y, w, h;
    } t[] = {
        { 0, 0, 0, 1 }, { 0, 0, 1, 0 }, { 320, 0, 1, 1 }, { 0, 240, 1, 1 },
        { 319, 0, 2, 1 }, { 0, 239, 1, 2 }, { 0, 0, 321, 1 }, { 0, 0, 1, 241 },
        { 1, 0, 0xffffffffu, 1 }, { 0, 1, 1, 0xffffffffu }, { 0xffffffffu, 0, 1, 1 },
    };
    unsigned i;

    up(90);
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CHECK_EQ(tft_write_rect(t[i].x, t[i].y, t[i].w, t[i].h, pix), TFT_ERR_BOUNDS);
        CHECK_EQ(tft_fill_rect(t[i].x, t[i].y, t[i].w, t[i].h, 0), TFT_ERR_BOUNDS);
        CHECK_EQ(tft_present_px32_rect(frame, 320 * 4, TFT_SRC_XRGB, t[i].x, t[i].y, t[i].w,
                                       t[i].h), TFT_ERR_BOUNDS);
    }
    CHECK_EQ(tft_write_rect(0, 0, 1, 1, rc_launder(NULL)), TFT_ERR_ARG);
    CHECK_EQ(tft_present_px32(rc_launder(NULL), 320 * 4, TFT_SRC_XRGB), TFT_ERR_ARG);
    CHECK_EQ(tft_present_px32(frame, 319 * 4, TFT_SRC_XRGB), TFT_ERR_ARG);
    CHECK_EQ(tft_present_px32(frame, 320 * 4 + 2, TFT_SRC_XRGB), TFT_ERR_ARG);
    CHECK_EQ(tft_present_px32(frame, 320 * 4, 2), TFT_ERR_ARG);
    CHECK_EQ(fake_n, 0);
}

TEST(chunk_boundaries)
{
    static const unsigned len[] = { 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 193, 320 };
    unsigned i;
    size_t at;

    up(90);
    pattern(320);
    for (i = 0; i < sizeof(len) / sizeof(len[0]); i++) {
        fake_clear();
        CHECK_EQ(tft_write_rect(0, 3, len[i], 1, pix), 0);
        at = check_window(0, len[i] - 1, 3, 3);
        check_pixels(at, pix, len[i]);
        /* 2 window parameters, the RAMWR byte, then one write per chunk. */
        CHECK_EQ(fake_writes, 2 * 2 + 1 + (len[i] + TFT_CHUNK_PIXELS - 1) / TFT_CHUNK_PIXELS);
        CHECK(fake_max_write <= 2 * TFT_CHUNK_PIXELS);
    }
    /* Chunks run across rows: 3 rows of 50 are 150 pixels, 3 chunks. */
    fake_clear();
    CHECK_EQ(tft_write_rect(10, 10, 50, 3, pix), 0);
    check_pixels(check_window(10, 59, 10, 12), pix, 150);
    CHECK_EQ(fake_writes, 2 * 2 + 1 + 3);
}

TEST(fill_rect)
{
    static uint16_t same[600];
    size_t i;

    for (i = 0; i < 600; i++) {
        same[i] = 0xf81f;
    }
    up(270);
    CHECK_EQ(tft_fill_rect(17, 200, 30, 20, 0xf81f), 0);
    check_pixels(check_window(17, 46, 200, 219), same, 600);
}

static void fill_frame(unsigned w, unsigned h)
{
    unsigned x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            frame[y * w + x] = (x * 0x010307u) ^ (y * 0x0b0503u) ^ 0x00804020u;
        }
    }
}

TEST(present_converts_each_pixel)
{
    size_t i;

    up(90);
    fill_frame(320, 240);
    for (i = 0; i < 320 * 240; i++) {
        pix[i] = tft_px32_to_565(frame[i], TFT_SRC_XBGR);
    }
    fake_clear();
    CHECK_EQ(tft_present_px32(frame, 320 * 4, TFT_SRC_XBGR), 0);
    check_pixels(check_window(0, 319, 0, 239), pix, 320 * 240);
}

/* Each dirty rectangle sends the same bytes as that region of a full present. */
TEST(dirty_rect_matches_full_present)
{
    static const struct {
        unsigned x, y, w, h;
    } t[] = { { 0, 0, 1, 1 }, { 13, 7, 100, 50 }, { 250, 200, 70, 40 }, { 0, 0, 320, 240 } };
    static uint8_t full[320 * 240 * 2];
    static uint32_t padded[240 * 330];
    unsigned i, x, y;
    size_t at;
    unsigned bad;

    up(90);
    fill_frame(320, 240);
    fake_clear();
    CHECK_EQ(tft_present_px32(frame, 320 * 4, TFT_SRC_XRGB), 0);
    at = check_window(0, 319, 0, 239);
    for (i = 0; i < sizeof(full); i++) {
        full[i] = (uint8_t)fake_ev[at + i].a;
    }
    /* The same picture in rows 330 pixels apart. */
    for (y = 0; y < 240; y++) {
        for (x = 0; x < 320; x++) {
            padded[y * 330 + x] = frame[y * 320 + x];
        }
    }
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        fake_clear();
        CHECK_EQ(tft_present_px32_rect(padded, 330 * 4, TFT_SRC_XRGB, t[i].x, t[i].y, t[i].w,
                                       t[i].h), 0);
        at = check_window(t[i].x, t[i].x + t[i].w - 1, t[i].y, t[i].y + t[i].h - 1);
        CHECK_EQ(fake_n, at + 2u * t[i].w * t[i].h + 1u);
        bad = 0;
        for (y = 0; y < t[i].h; y++) {
            for (x = 0; x < 2 * t[i].w; x++) {
                bad += fake_ev[at + 2 * (y * t[i].w) + x].a !=
                       full[2 * ((t[i].y + y) * 320 + t[i].x) + x];
            }
        }
        CHECK_EQ(bad, 0);
    }
}

TEST(sleep_and_display_transcripts)
{
    static const ev_t want[] = {
        PIN(BL, 0), CSL, C(0x10), CSH, MS(120),     /* SLPIN */
        CSL, C(0x11), CSH, MS(120), PIN(BL, 1),     /* SLPOUT */
        PIN(BL, 0), CSL, C(0x28), CSH,              /* DISPOFF */
        CSL, C(0x29), CSH, PIN(BL, 1),              /* DISPON */
    };
    tft_config_t c = config();

    c.backlight_pin = BL;
    CHECK_EQ(init_with(&c), 0);
    fake_clear();
    CHECK_EQ(tft_sleep(1), 0);
    CHECK_EQ(tft_sleep(0), 0);
    CHECK_EQ(tft_display(0), 0);
    CHECK_EQ(tft_display(1), 0);
    EXPECT(want);
}

TEST(read_id)
{
    static const ev_t want[] = {
        RATE(TFT_READ_HZ), CSL, C(0x04), { EV_READ, 0, 4 }, CSH, RATE(32000000),
    };
    uint32_t id = 1;

    up(90);
    CHECK_EQ(tft_read_id(&id), TFT_ERR_NO_PANEL);   /* MISO reads zeros */
    CHECK_EQ(id, 0);
    EXPECT(want);
    memset(fake_rx, 0xff, sizeof(fake_rx));
    CHECK_EQ(tft_read_id(&id), TFT_ERR_NO_PANEL);   /* floating high */
    fake_rx[0] = 0x00;
    fake_rx[1] = 0x00;
    fake_rx[2] = 0x93;
    fake_rx[3] = 0x41;
    CHECK_EQ(tft_read_id(&id), 0);
    CHECK_EQ(id, 0x9341);
    CHECK_EQ(tft_get_hz(), 31250000);
    CHECK_EQ(tft_read_id(rc_launder(NULL)), TFT_ERR_ARG);
}

TEST(bus_error_releases_chip_select)
{
    up(90);
    fake_write_fail = 1;
    CHECK_EQ(tft_fill_rect(0, 0, 10, 10, 0), TFT_ERR_BUS);
    CHECK_EQ(fake_open, -1);
    fake_write_fail = 0;
    CHECK_EQ(tft_fill_rect(0, 0, 10, 10, 0), 0);
}
