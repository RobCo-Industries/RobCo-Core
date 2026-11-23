/*
 * fb_adopt() and console_adopt() on a RAM buffer. The test builds the
 * framebuffer and console drivers in, with the mailbox, MMU and UART
 * stubbed, and reaches their static state directly.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/fb.c"
#include "../../src/gfx/console.c"
#include "../../src/gfx/font8x16_fixedsys.c"
#include "../../src/gfx/schemes.c"

#include <stdio.h>
#include <string.h>

#include <robco/bootinfo.h>

#include "rc_test.h"

int mbox_call(uint32_t *buf)
{
    (void)buf;
    return -1;
}

static unsigned map_calls;

int mmu_map(uint32_t start, uint32_t size, unsigned attr)
{
    (void)start;
    (void)size;
    (void)attr;
    map_calls++;
    return 0;
}

void uart_putc(char c)
{
    (void)c;
}

/* 12 x 4 cells, with 16 bytes of padding at the end of each row. */
#define SW      96u
#define SH      64u
#define PITCH   (SW * 4u + 16u)
#define BYTES   (PITCH * SH)
#define FILL    0xa5u

static uint32_t screen_words[BYTES / 4u];
static uint8_t *const screen = (uint8_t *)screen_words;
static uint8_t before[BYTES];

static fb_info_t good(uint32_t order)
{
    fb_info_t i = { screen, BYTES, SW, SH, PITCH, 32, order };

    return i;
}

/* Back to "no driver has run": the state a fresh image starts in. */
static void reset(void)
{
    fb_ready = 0;
    k_memset(&fb_state, 0, sizeof(fb_state));
    ready = 0;
    memset(screen, FILL, BYTES);
}

static int same_info(const fb_info_t *a, const fb_info_t *b)
{
    return a->addr == b->addr && a->size == b->size && a->width == b->width &&
           a->height == b->height && a->pitch == b->pitch && a->depth == b->depth &&
           a->order == b->order;
}

TEST(fb_adopt_refuses_each_bad_field_and_keeps_state)
{
    fb_info_t prev = good(FB_ORDER_BGR);
    fb_info_t b;
    struct {
        const char *what;
        int rc;
    } cases[] = {
        { "addr null", FB_ERR_ADDR },  { "addr odd", FB_ERR_ADDR },
        { "addr wraps", FB_ERR_ADDR }, { "width 0", FB_ERR_SIZE },
        { "height 0", FB_ERR_SIZE },   { "depth 16", FB_ERR_DEPTH },
        { "depth 24", FB_ERR_DEPTH },  { "pitch small", FB_ERR_BUFFER },
        { "pitch odd", FB_ERR_BUFFER }, { "pitch 0", FB_ERR_BUFFER },
        { "width huge", FB_ERR_BUFFER }, { "size small", FB_ERR_BUFFER },
        { "size 0", FB_ERR_BUFFER },   { "order 2", FB_ERR_ORDER },
    };
    unsigned i;

    reset();
    CHECK_EQ(fb_adopt(rc_launder(NULL)), FB_ERR_ADDR);
    CHECK(fb_get() == NULL);
    CHECK_EQ(fb_adopt(&prev), 0);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        b = good(FB_ORDER_RGB);
        switch (i) {
        case 0: b.addr = NULL; break;
        case 1: b.addr = screen + 2; break;
        case 2: b.addr = (uint8_t *)(UINTPTR_MAX - 0xfffu); b.size = 0x2000; break;
        case 3: b.width = 0; break;
        case 4: b.height = 0; break;
        case 5: b.depth = 16; break;
        case 6: b.depth = 24; break;
        case 7: b.pitch = SW * 4u - 4u; break;
        case 8: b.pitch = PITCH + 2u; b.size = BYTES + 2u * SH; break;
        case 9: b.pitch = 0; break;
        case 10: b.width = 0x40000001u; break;
        case 11: b.size = PITCH * SH - 1u; break;
        case 12: b.size = 0; break;
        case 13: b.order = 2; break;
        default: break;
        }
        if (fb_adopt(&b) != cases[i].rc) {
            RC_FAIL("%s: rc %d, want %d", cases[i].what, fb_adopt(&b), cases[i].rc);
        }
        CHECK(fb_get() != NULL && same_info(fb_get(), &prev));
    }
    /* Each case changes one field: the base passes. */
    b = good(FB_ORDER_RGB);
    CHECK_EQ(fb_adopt(&b), 0);
    CHECK(same_info(fb_get(), &b));
}

TEST(fb_adopt_writes_no_pixel_and_maps_the_buffer)
{
    fb_info_t i = good(FB_ORDER_RGB);
    unsigned calls;

    reset();
    memcpy(before, screen, BYTES);
    calls = map_calls;
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(map_calls, calls + 1);
    CHECK_MEM(screen, before, BYTES);
}

/* Draws through every fb_* call and checks the bytes in memory. */
static void check_drawing(uint32_t order, const uint8_t px[4])
{
    fb_info_t i = good(order);
    uint32_t c, x, y;
    const uint8_t *p;

    reset();
    CHECK_EQ(fb_adopt(&i), 0);
    c = fb_rgb(0x11, 0x22, 0x33);
    memcpy(before, screen, BYTES);

    fb_put_pixel(5, 7, c);
    CHECK_MEM(screen + 7 * PITCH + 5 * 4, px, 4);
    memcpy(before + 7 * PITCH + 5 * 4, px, 4);
    CHECK_MEM(screen, before, BYTES);

    /* Clipped at the right edge: the row padding stays. */
    fb_fill_rect(SW - 2, 10, 8, 2, c);
    for (y = 10; y < 12; y++) {
        for (x = SW - 2; x < SW; x++) {
            memcpy(before + y * PITCH + x * 4, px, 4);
        }
    }
    CHECK_MEM(screen, before, BYTES);

    fb_clear(c);
    for (y = 0; y < SH; y++) {
        for (x = 0; x < SW; x++) {
            p = screen + y * PITCH + x * 4;
            if (memcmp(p, px, 4) != 0) {
                RC_FAIL("pixel %u,%u", (unsigned)x, (unsigned)y);
                return;
            }
        }
        for (x = SW * 4; x < PITCH; x++) {
            CHECK_EQ(screen[y * PITCH + x], FILL);
        }
    }
}

TEST(drawing_after_adopt_rgb)
{
    static const uint8_t px[4] = { 0x11, 0x22, 0x33, 0xff };

    check_drawing(FB_ORDER_RGB, px);
}

TEST(drawing_after_adopt_bgr)
{
    static const uint8_t px[4] = { 0x33, 0x22, 0x11, 0xff };

    check_drawing(FB_ORDER_BGR, px);
}

TEST(bootinfo_fb_info_fills_from_the_block)
{
    robco_bootinfo_t bi;
    fb_info_t out;

    memset(&bi, 0, sizeof(bi));
    CHECK_EQ(bootinfo_fb_info(rc_launder(NULL), &out), BOOTINFO_E_NO_FB);
    bi.fb_addr = 0x3c100000u;
    CHECK_EQ(bootinfo_fb_info(&bi, &out), BOOTINFO_E_NO_FB);
    bi.caps = ROBCO_CAP_FB;
    bi.fb_size = 1228800;
    bi.fb_width = 640;
    bi.fb_height = 480;
    bi.fb_pitch = 2560;
    bi.fb_depth = 32;
    bi.fb_order = FB_ORDER_RGB;
    CHECK_EQ(bootinfo_fb_info(&bi, &out), BOOTINFO_OK);
    CHECK_EQ((uintptr_t)out.addr, 0x3c100000u);
    CHECK_EQ(out.size, 1228800);
    CHECK_EQ(out.width, 640);
    CHECK_EQ(out.height, 480);
    CHECK_EQ(out.pitch, 2560);
    CHECK_EQ(out.depth, 32);
    CHECK_EQ(out.order, FB_ORDER_RGB);
    bi.fb_addr = 0;
    CHECK_EQ(bootinfo_fb_info(&bi, &out), BOOTINFO_E_NO_FB);
}

/* The earlier stage: a console in green with two lines, then a cursor at 2,1. */
static void earlier_stage(void)
{
    fb_info_t i = good(FB_ORDER_RGB);

    reset();
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_init(CONSOLE_SCHEME_GREEN), 0);
    console_puts("first\nAB");
    ready = 0;
    fb_ready = 0;
}

TEST(console_adopt_refuses_bad_grids_without_drawing)
{
    fb_info_t i = good(FB_ORDER_RGB);
    fb_info_t big = { screen, 0xffffff00u, 8u * 101u, 16u * 41u, 8u * 101u * 4u, 32,
                      FB_ORDER_RGB };

    earlier_stage();
    CHECK_EQ(console_adopt(12, 4, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_NO_FB);
    CHECK_EQ(fb_adopt(&i), 0);
    memcpy(before, screen, BYTES);
    CHECK_EQ(console_adopt(12, 4, 2, 1, CONSOLE_SCHEME_COUNT), CONSOLE_ERR_SCHEME);
    CHECK_EQ(console_adopt(0, 4, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_TOO_BIG);
    CHECK_EQ(console_adopt(12, 0, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_TOO_BIG);
    CHECK_EQ(console_adopt(13, 4, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_TOO_BIG);
    CHECK_EQ(console_adopt(12, 5, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_TOO_BIG);
    CHECK_EQ(console_adopt(12, 4, 12, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_CURSOR);
    CHECK_EQ(console_adopt(12, 4, 0, 4, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_CURSOR);
    CHECK_EQ(console_cols(), 0);
    console_puts("x");
    CHECK_MEM(screen, before, BYTES);

    /* Larger than the cell buffer. The mode is never drawn to. */
    CHECK_EQ(fb_adopt(&big), 0);
    CHECK_EQ(console_adopt(101, 40, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_TOO_BIG);
    CHECK_EQ(console_adopt(100, 41, 0, 0, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_TOO_BIG);
    CHECK_EQ(console_cols(), 0);
    CHECK_MEM(screen, before, BYTES);

    /* A refusal keeps a console already running. */
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_adopt(12, 4, 2, 1, CONSOLE_SCHEME_BLUE), 0);
    CHECK_EQ(console_adopt(12, 4, 12, 1, CONSOLE_SCHEME_AMBER), CONSOLE_ERR_CURSOR);
    CHECK_EQ(console_cols(), 12);
    CHECK_EQ(console_scheme(), CONSOLE_SCHEME_BLUE);
    CHECK_MEM(screen, before, BYTES);
}

/* 1 if cell col,row shows glyph `ch` in fg on bg. */
static int cell_is(unsigned col, unsigned row, uint8_t ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *g = font8x16_glyph(ch);
    const uint32_t *p;
    unsigned x, y;

    for (y = 0; y < FONT_H; y++) {
        p = (const uint32_t *)(const void *)(screen + (row * FONT_H + y) * PITCH) + col * FONT_W;
        for (x = 0; x < FONT_W; x++) {
            if (p[x] != ((g[y] & (0x80u >> x)) ? fg : bg)) {
                return 0;
            }
        }
    }
    return 1;
}

static void copy_cell(uint8_t *dst, const uint8_t *src, unsigned col, unsigned row)
{
    unsigned y;
    size_t at;

    for (y = 0; y < FONT_H; y++) {
        at = (row * FONT_H + y) * PITCH + col * FONT_W * 4;
        memcpy(dst + at, src + at, FONT_W * 4);
    }
}

TEST(print_after_adopt_starts_at_the_cursor_in_the_scheme)
{
    fb_info_t i = good(FB_ORDER_RGB);
    const console_scheme_t *s = console_scheme_info(CONSOLE_SCHEME_AMBER);
    uint32_t fg, bg;
    unsigned col, row;

    earlier_stage();
    memcpy(before, screen, BYTES);
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_adopt(12, 4, 2, 1, CONSOLE_SCHEME_AMBER), 0);
    CHECK_MEM(screen, before, BYTES);
    CHECK_EQ(console_cols(), 12);
    CHECK_EQ(console_rows(), 4);
    console_get_cursor(&col, &row);
    CHECK_EQ(col, 2);
    CHECK_EQ(row, 1);
    CHECK_EQ(console_scheme(), CONSOLE_SCHEME_AMBER);

    console_puts("Z");
    fg = fb_rgb(s->normal.r, s->normal.g, s->normal.b);
    bg = fb_rgb(s->bg.r, s->bg.g, s->bg.b);
    CHECK(cell_is(2, 1, 'Z', fg, bg));
    /* Everything else, the first line above included, is as the earlier stage left it. */
    copy_cell(before, screen, 2, 1);
    CHECK_MEM(screen, before, BYTES);
    console_get_cursor(&col, &row);
    CHECK_EQ(col, 3);
    CHECK_EQ(row, 1);
}

TEST(scroll_after_adopt_moves_the_adopted_pixels_one_cell_up)
{
    fb_info_t i = good(FB_ORDER_RGB);
    const console_scheme_t *s = console_scheme_info(CONSOLE_SCHEME_AMBER);
    uint32_t bg;
    unsigned y, x;
    const uint32_t *p;

    earlier_stage();
    console_set_cursor(0, 0);
    memcpy(before, screen, BYTES);
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_adopt(12, 4, 0, 3, CONSOLE_SCHEME_AMBER), 0);
    console_puts("\n");
    for (y = 0; y + FONT_H < SH; y++) {
        CHECK_MEM(screen + y * PITCH, before + (y + FONT_H) * PITCH, SW * 4);
    }
    bg = fb_rgb(s->bg.r, s->bg.g, s->bg.b);
    for (y = SH - FONT_H; y < SH; y++) {
        p = (const uint32_t *)(const void *)(screen + y * PITCH);
        for (x = 0; x < SW; x++) {
            if (p[x] != bg) {
                RC_FAIL("row %u col %u not background", y, x);
                return;
            }
        }
    }
}

TEST(scroll_of_a_short_grid_blanks_its_new_row)
{
    fb_info_t i = good(FB_ORDER_RGB);
    const console_scheme_t *s = console_scheme_info(CONSOLE_SCHEME_AMBER);
    uint32_t bg;
    unsigned y, x;
    const uint32_t *p;

    earlier_stage();
    CHECK_EQ(fb_adopt(&i), 0);
    /* Three rows of four: the row below the grid holds the earlier stage's pixels. */
    CHECK_EQ(console_adopt(12, 3, 0, 2, CONSOLE_SCHEME_AMBER), 0);
    fb_fill_rect(0, 3 * FONT_H, SW, FONT_H, 0x12345678u);
    console_puts("\n");
    bg = fb_rgb(s->bg.r, s->bg.g, s->bg.b);
    for (y = 2 * FONT_H; y < 3 * FONT_H; y++) {
        p = (const uint32_t *)(const void *)(screen + y * PITCH);
        for (x = 0; x < SW; x++) {
            if (p[x] != bg) {
                RC_FAIL("row %u col %u not background", y, x);
                return;
            }
        }
    }
}

static char dump[1024];
static size_t dump_len;

static void dump_out(char c)
{
    if (dump_len + 1 < sizeof(dump)) {
        dump[dump_len++] = c;
        dump[dump_len] = '\0';
    }
}

static const char *take_dump(void)
{
    dump_len = 0;
    dump[0] = '\0';
    console_dump(dump_out);
    return dump;
}

TEST(dump_shows_adopted_cells_as_unknown_until_written)
{
    fb_info_t i = good(FB_ORDER_RGB);

    earlier_stage();
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_adopt(12, 4, 2, 1, CONSOLE_SCHEME_AMBER), 0);
    CHECK_STR(take_dump(),
              "@RC stage=host ev=screen cols=12 rows=4 cx=2 cy=1 scheme=amber\n"
              "|############\n|############\n|############\n|############\n"
              "@RC stage=host ev=screen_end\n");
    console_puts("Z \n");
    console_puts("ok");
    CHECK_STR(take_dump(),
              "@RC stage=host ev=screen cols=12 rows=4 cx=2 cy=2 scheme=amber\n"
              "|############\n|##Z ########\n|ok##########\n|############\n"
              "@RC stage=host ev=screen_end\n");
    /* A scroll moves the cells with the pixels; the new row is known and blank. */
    console_set_cursor(0, 3);
    console_puts("\n");
    CHECK_STR(take_dump(),
              "@RC stage=host ev=screen cols=12 rows=4 cx=0 cy=3 scheme=amber\n"
              "|##Z ########\n|ok##########\n|############\n|\n"
              "@RC stage=host ev=screen_end\n");
    console_clear();
    CHECK_STR(take_dump(),
              "@RC stage=host ev=screen cols=12 rows=4 cx=0 cy=0 scheme=amber\n"
              "|\n|\n|\n|\n"
              "@RC stage=host ev=screen_end\n");
}

TEST(scheme_change_and_cursor_leave_unknown_cells_alone)
{
    fb_info_t i = good(FB_ORDER_RGB);
    const console_scheme_t *s = console_scheme_info(CONSOLE_SCHEME_BLUE);

    earlier_stage();
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_adopt(12, 4, 0, 1, CONSOLE_SCHEME_AMBER), 0);
    console_puts("Q");
    memcpy(before, screen, BYTES);
    console_set_scheme(CONSOLE_SCHEME_BLUE);
    CHECK(cell_is(0, 1, 'Q', fb_rgb(s->normal.r, s->normal.g, s->normal.b),
                  fb_rgb(s->bg.r, s->bg.g, s->bg.b)));
    copy_cell(before, screen, 0, 1);
    CHECK_MEM(screen, before, BYTES);
    /* The cursor now sits on an unknown cell: showing it draws nothing. */
    console_cursor_show(1);
    CHECK_MEM(screen, before, BYTES);
}

/*
 * A screen larger than the grid in both directions: 100 x 70 pixels, so
 * console_init() makes a 12 x 4 grid of 96 x 64 with a margin right and
 * below. Every word starts as a distinct pattern value.
 */
#define BW      100u
#define BH      70u
#define BPITCH  (BW * 4u + 16u)
#define BWORDS  (BPITCH / 4u * BH)

static uint32_t big_words[BWORDS];
static uint32_t big_before[BWORDS];

static fb_info_t big_info(void)
{
    fb_info_t i = { (uint8_t *)big_words, BPITCH * BH, BW, BH, BPITCH, 32, FB_ORDER_RGB };

    return i;
}

static void big_pattern(void)
{
    uint32_t n;

    for (n = 0; n < BWORDS; n++) {
        big_words[n] = 0x80000000u | (n * 2654435761u >> 1);
    }
}

static uint32_t big_px(const uint32_t *w, uint32_t x, uint32_t y)
{
    return w[y * (BPITCH / 4u) + x];
}

/*
 * After `k` scrolls of a gw x gh grid at 0,0: outside the grid, every word
 * (row padding included) equals big_before; inside, row y holds
 * big_before's row y + k * FONT_H, and the bottom k cell rows are `bg`.
 */
static int big_check(uint32_t gw, uint32_t gh, uint32_t k, uint32_t bg)
{
    uint32_t x, y, got, want;

    for (y = 0; y < BH; y++) {
        for (x = 0; x < BPITCH / 4u; x++) {
            got = big_px(big_words, x, y);
            if (x >= gw || y >= gh) {
                want = big_px(big_before, x, y);
            } else if (y + k * FONT_H < gh) {
                want = big_px(big_before, x, y + k * FONT_H);
            } else {
                want = bg;
            }
            if (got != want) {
                RC_FAIL("scroll %u: pixel %u,%u is %08x, want %08x", (unsigned)k, (unsigned)x,
                        (unsigned)y, (unsigned)got, (unsigned)want);
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t amber_bg(void)
{
    const console_scheme_t *s = console_scheme_info(CONSOLE_SCHEME_AMBER);

    return fb_rgb(s->bg.r, s->bg.g, s->bg.b);
}

TEST(scroll_of_an_adopted_small_grid_moves_only_the_grid)
{
    fb_info_t i = big_info();
    uint32_t k;

    reset();
    big_pattern();
    CHECK_EQ(fb_adopt(&i), 0);
    /* 9 x 3 cells: 72 x 48 pixels of the 100 x 70 screen. */
    CHECK_EQ(console_adopt(9, 3, 0, 2, CONSOLE_SCHEME_AMBER), 0);
    memcpy(big_before, big_words, sizeof(big_words));
    for (k = 1; k <= 4; k++) {
        console_puts("\n");
        if (!big_check(9 * FONT_W, 3 * FONT_H, k, amber_bg())) {
            return;
        }
    }
}

TEST(scroll_of_an_initialised_small_grid_moves_only_the_grid)
{
    fb_info_t i = big_info();
    uint32_t k, x, y;

    reset();
    big_pattern();
    CHECK_EQ(fb_adopt(&i), 0);
    memcpy(big_before, big_words, sizeof(big_words));
    CHECK_EQ(console_init(CONSOLE_SCHEME_AMBER), 0);
    CHECK_EQ(console_cols(), 12);
    CHECK_EQ(console_rows(), 4);
    /* console_init() clears the grid and nothing else. */
    if (!big_check(96, 64, 4, amber_bg())) {
        return;
    }
    /* Pattern the grid again, so each scroll shows where its rows went. */
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 96; x++) {
            big_words[y * (BPITCH / 4u) + x] = big_px(big_before, x, y);
        }
    }
    memcpy(big_before, big_words, sizeof(big_words));
    console_set_cursor(0, 3);
    for (k = 1; k <= 3; k++) {
        console_puts("\n");
        if (!big_check(96, 64, k, amber_bg())) {
            return;
        }
    }
}

TEST(clear_scheme_and_cursor_stay_inside_a_small_grid)
{
    fb_info_t i = big_info();

    reset();
    big_pattern();
    CHECK_EQ(fb_adopt(&i), 0);
    CHECK_EQ(console_adopt(9, 3, 0, 2, CONSOLE_SCHEME_AMBER), 0);
    memcpy(big_before, big_words, sizeof(big_words));
    console_cursor_show(1);
    console_puts("abcdefghij");
    console_set_cursor(8, 2);
    console_set_scheme(CONSOLE_SCHEME_BLUE);
    console_set_scheme(CONSOLE_SCHEME_AMBER);
    console_clear();
    console_cursor_show(0);
    /* k = 3 scrolls of a 3-row grid: the whole grid is background. */
    big_check(9 * FONT_W, 3 * FONT_H, 3, amber_bg());
}

/* Copies big_before's rect x,y,w,h moved up by `p` into big_before, filling the rest with bg. */
static void model_scroll(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t p, uint32_t bg)
{
    uint32_t i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            big_before[(y + j) * (BPITCH / 4u) + x + i] =
                j + p < h ? big_before[(y + j + p) * (BPITCH / 4u) + x + i] : bg;
        }
    }
}

TEST(fb_scroll_rect_moves_the_clipped_rectangle_only)
{
    fb_info_t i = big_info();

    reset();
    big_pattern();
    memcpy(big_before, big_words, sizeof(big_words));
    /* No framebuffer: nothing. */
    fb_scroll_rect(0, 0, BW, BH, 1, 7);
    CHECK_MEM(big_words, big_before, sizeof(big_words));

    CHECK_EQ(fb_adopt(&i), 0);
    fb_scroll_rect(3, 5, 20, 11, 4, 7);
    model_scroll(3, 5, 20, 11, 4, 7);
    CHECK_MEM(big_words, big_before, sizeof(big_words));

    /* Clipped right and below: the row padding stays. */
    fb_scroll_rect(90, 60, 50, 50, 3, 9);
    model_scroll(90, 60, BW - 90, BH - 60, 3, 9);
    CHECK_MEM(big_words, big_before, sizeof(big_words));

    /* pixels >= h fills the rectangle; 0 pixels, empty or off-screen rectangles do nothing. */
    fb_scroll_rect(10, 10, 5, 4, 4, 11);
    model_scroll(10, 10, 5, 4, 4, 11);
    fb_scroll_rect(20, 20, 5, 4, 0, 12);
    fb_scroll_rect(20, 20, 0, 4, 1, 12);
    fb_scroll_rect(20, 20, 5, 0, 1, 12);
    fb_scroll_rect(BW, 0, 5, 4, 1, 12);
    fb_scroll_rect(0, BH, 5, 4, 1, 12);
    CHECK_MEM(big_words, big_before, sizeof(big_words));

    /* fb_scroll_up() is the whole screen. */
    fb_scroll_up(17, 13);
    model_scroll(0, 0, BW, BH, 17, 13);
    CHECK_MEM(big_words, big_before, sizeof(big_words));
}
