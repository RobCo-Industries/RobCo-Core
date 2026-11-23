/* The ILI9341 driver end to end through a panel model, and the model itself. */
#include <stdint.h>

#include "ili9341_model.h"
#include "rc_test.h"
#include "tft_fake.h"

static model_t model;
static uint16_t pic[320 * 240];
static uint16_t view[320 * 240];
static uint32_t frame[320 * 240];
static uint32_t frame2[320 * 240];

/* Plays the fake transcript into the model, then clears it. */
static void play(int reset_pin)
{
    size_t i;

    CHECK_EQ(fake_overflow, 0);
    for (i = 0; i < fake_n; i++) {
        switch (fake_ev[i].kind) {
        case EV_CMD: model_byte(&model, 0, (uint8_t)fake_ev[i].a); break;
        case EV_DATA: model_byte(&model, 1, (uint8_t)fake_ev[i].a); break;
        case EV_PIN:
            if (fake_ev[i].a == reset_pin) {
                model_reset_pin(&model, (int)fake_ev[i].b);
            }
            break;
        default: break;
        }
    }
    fake_clear();
}

static void cmd(uint8_t c)
{
    model_byte(&model, 0, c);
}

static void data(uint8_t d)
{
    model_byte(&model, 1, d);
}

static void px(uint16_t v)
{
    data((uint8_t)(v >> 8));
    data((uint8_t)v);
}

static void window(unsigned sc, unsigned ec, unsigned sp, unsigned ep)
{
    cmd(0x2a);
    data((uint8_t)(sc >> 8));
    data((uint8_t)sc);
    data((uint8_t)(ec >> 8));
    data((uint8_t)ec);
    cmd(0x2b);
    data((uint8_t)(sp >> 8));
    data((uint8_t)sp);
    data((uint8_t)(ep >> 8));
    data((uint8_t)ep);
}

/* --- The model on hand-made transcripts. --- */

TEST(model_window_wraps_inside_itself)
{
    model_init(&model);
    cmd(0x3a);
    data(0x55);
    window(10, 11, 20, 21);
    cmd(0x2c);
    px(1);
    px(2);
    px(3);
    px(4);
    px(5);      /* past EP: back to SC, SP */
    px(6);
    CHECK_EQ(model.mem[20][10], 5);
    CHECK_EQ(model.mem[20][11], 6);
    CHECK_EQ(model.mem[21][10], 3);
    CHECK_EQ(model.mem[21][11], 4);
    CHECK_EQ(model.mem[20][12], 0);
    CHECK_EQ(model.mem[22][10], 0);
    CHECK_EQ(model.pixels, 6);
}

TEST(model_ignores_data_before_ramwr)
{
    model_init(&model);
    cmd(0x3a);
    data(0x55);
    cmd(0x00);  /* NOP */
    px(0xffff);
    cmd(0x29);  /* DISPON takes no parameter */
    px(0xffff);
    CHECK_EQ(model.pixels, 0);
    CHECK_EQ(model.dropped, 4);
    CHECK_EQ(model.mem[0][0], 0);
    cmd(0x2c);
    px(0x1234);
    CHECK_EQ(model.mem[0][0], 0x1234);
    /* A command ends the memory write: the next data are not pixels. */
    cmd(0x00);
    px(0x5678);
    CHECK_EQ(model.mem[0][1], 0);
}

TEST(model_stores_pixels_while_asleep)
{
    model_init(&model);
    cmd(0x3a);
    data(0x55);
    cmd(0x2c);
    px(0xbeef);
    CHECK_EQ(model.asleep, 1);
    CHECK_EQ(model.display_on, 0);
    CHECK_EQ(model.mem[0][0], 0xbeef);
}

TEST(model_needs_16_bit_format)
{
    model_init(&model);     /* COLMOD 0x66 after reset */
    cmd(0x2c);
    px(0xbeef);
    CHECK_EQ(model.mem[0][0], 0);
    CHECK_EQ(model.pixels, 0);
}

TEST(model_address_order_bits)
{
    model_init(&model);
    cmd(0x3a);
    data(0x55);
    cmd(0x36);
    data(0x40);         /* MX */
    cmd(0x2c);
    px(7);
    CHECK_EQ(model.mem[0][239], 7);
    cmd(0x36);
    data(0x80);         /* MY */
    cmd(0x2c);
    px(8);
    CHECK_EQ(model.mem[319][0], 8);
    cmd(0x36);
    data(0x20);         /* MV: the column address walks down the rows */
    window(5, 6, 9, 9);
    cmd(0x2c);
    px(9);
    px(10);
    CHECK_EQ(model.mem[5][9], 9);
    CHECK_EQ(model.mem[6][9], 10);
}

/* --- The driver through the model. --- */

static void up(unsigned rotation)
{
    tft_config_t c = TFT_CONFIG_DEFAULT;

    c.rotation = rotation;
    fake_reset(c.dc_pin);
    model_init(&model);
    CHECK_EQ(tft_init(&c), 0);
    play(c.reset_pin);
    CHECK_EQ(model.asleep, 0);
    CHECK_EQ(model.display_on, 1);
    CHECK_EQ(model.colmod, 0x55);
    CHECK_EQ(model.bgr, 1);
}

/* A picture where every pixel differs from its neighbours and its mirror. */
static void make_picture(unsigned w, unsigned h)
{
    unsigned x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            pic[y * w + x] = (uint16_t)(x + y * 331u + 1u);
        }
    }
}

static unsigned mismatches(const uint16_t *a, const uint16_t *b, size_t n)
{
    unsigned bad = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        bad += a[i] != b[i];
    }
    return bad;
}

TEST(each_rotation_shows_the_picture_upright)
{
    static const unsigned rot[] = { 0, 90, 180, 270 };
    unsigned i, w, h;

    for (i = 0; i < 4; i++) {
        up(rot[i]);
        w = tft_width();
        h = tft_height();
        make_picture(w, h);
        CHECK_EQ(tft_write_rect(0, 0, w, h, pic), 0);
        play(4);
        CHECK_EQ(model_view(&model, rot[i], view), w);
        CHECK_EQ(mismatches(view, pic, (size_t)w * h), 0);
        CHECK_EQ(model.pixels, w * h);
    }
}

TEST(small_rects_land_in_place)
{
    static const uint16_t four[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };
    static const uint16_t zero[320 * 240];

    up(90);
    CHECK_EQ(tft_fill_rect(0, 0, 320, 240, 0), 0);
    CHECK_EQ(tft_write_rect(0, 0, 1, 1, four), 0);
    CHECK_EQ(tft_write_rect(319, 239, 1, 1, four + 1), 0);
    CHECK_EQ(tft_write_rect(100, 50, 2, 2, four), 0);
    play(4);
    model_view(&model, 90, view);
    CHECK_EQ(view[0], 0x1111);
    CHECK_EQ(view[239 * 320 + 319], 0x2222);
    CHECK_EQ(view[50 * 320 + 100], 0x1111);
    CHECK_EQ(view[50 * 320 + 101], 0x2222);
    CHECK_EQ(view[51 * 320 + 100], 0x3333);
    CHECK_EQ(view[51 * 320 + 101], 0x4444);
    CHECK_EQ(320 * 240 - mismatches(view, zero, 320 * 240), 320 * 240 - 6);
}

static void fill32(uint32_t *f, uint32_t seed)
{
    unsigned i;

    for (i = 0; i < 320 * 240; i++) {
        f[i] = (i * 2654435761u) ^ seed;
    }
}

/* A full present, then dirty rectangles from a second frame: the composed picture. */
TEST(dirty_rects_compose)
{
    static const struct {
        unsigned x, y, w, h;
    } t[] = { { 0, 0, 10, 10 }, { 100, 100, 50, 30 }, { 310, 230, 10, 10 }, { 0, 200, 320, 1 } };
    static uint16_t want[320 * 240];
    unsigned i, x, y;

    up(270);
    fill32(frame, 0x00123456u);
    fill32(frame2, 0x00fedcbau);
    CHECK_EQ(tft_present_px32(frame, 320 * 4, TFT_SRC_XRGB), 0);
    for (i = 0; i < 320 * 240; i++) {
        want[i] = tft_px32_to_565(frame[i], TFT_SRC_XRGB);
    }
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CHECK_EQ(tft_present_px32_rect(frame2, 320 * 4, TFT_SRC_XRGB, t[i].x, t[i].y, t[i].w,
                                       t[i].h), 0);
        for (y = t[i].y; y < t[i].y + t[i].h; y++) {
            for (x = t[i].x; x < t[i].x + t[i].w; x++) {
                want[y * 320 + x] = tft_px32_to_565(frame2[y * 320 + x], TFT_SRC_XRGB);
            }
        }
    }
    play(4);
    model_view(&model, 270, view);
    CHECK_EQ(mismatches(view, want, 320 * 240), 0);
}

TEST(sleep_keeps_memory)
{
    up(90);
    CHECK_EQ(tft_sleep(1), 0);
    CHECK_EQ(tft_fill_rect(5, 5, 1, 1, 0xabcd), 0);
    CHECK_EQ(tft_display(0), 0);
    play(4);
    CHECK_EQ(model.asleep, 1);
    CHECK_EQ(model.display_on, 0);
    model_view(&model, 90, view);
    CHECK_EQ(view[5 * 320 + 5], 0xabcd);
    CHECK_EQ(tft_sleep(0), 0);
    CHECK_EQ(tft_display(1), 0);
    play(4);
    CHECK_EQ(model.asleep, 0);
    CHECK_EQ(model.display_on, 1);
}
