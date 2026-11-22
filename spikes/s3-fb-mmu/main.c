#include <stdint.h>

#include "board.h"
#include "fb.h"
#include "k_printf.h"
#include "mbox.h"
#include "mmu.h"
#include "systimer.h"
#include "uart_pl011.h"

#define FB_W        640
#define FB_H        480
#define FB_DEPTH    32
#define FILL_FRAMES 20
#define SQUARE      16

void spike_main(uint32_t r0, uint32_t r1, uint32_t r2);

static void halt(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void draw_pattern(const struct fb_info *fb)
{
    /* Left to right: white, yellow, cyan, green, magenta, red, blue, black. */
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
    };
    uint32_t bar_px[8];
    uint32_t stride = fb->pitch / 4;
    uint32_t w = fb->width;
    uint32_t h = fb->height;
    uint32_t white = fb_rgb(fb->order, 255, 255, 255);
    uint32_t red = fb_rgb(fb->order, 255, 0, 0);
    uint32_t green = fb_rgb(fb->order, 0, 255, 0);
    uint32_t blue = fb_rgb(fb->order, 0, 0, 255);
    uint32_t *row;
    uint32_t x, y, i;

    for (i = 0; i < 8; i++) {
        bar_px[i] = fb_rgb(fb->order, bars[i][0], bars[i][1], bars[i][2]);
    }

    for (y = 0; y < h; y++) {
        row = (uint32_t *)(uintptr_t)fb->addr + y * stride;
        for (x = 0; x < w; x++) {
            uint32_t px = bar_px[(x * 8) / w];
            if (y < SQUARE && x < SQUARE) {
                px = red;
            } else if (y < SQUARE && x >= w - SQUARE) {
                px = green;
            } else if (y >= h - SQUARE && x < SQUARE) {
                px = blue;
            }
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                px = white;
            }
            row[x] = px;
        }
    }
    mmu_clean_dcache_range(fb->addr, fb->addr + stride * 4 * h);
}

/* Fills the whole buffer `frames` times. Returns elapsed microseconds. */
static uint32_t fill_frames(const struct fb_info *fb, uint32_t frames)
{
    uint32_t words = (fb->pitch / 4) * fb->height;
    uint32_t *base = (uint32_t *)(uintptr_t)fb->addr;
    uint32_t start = systimer_us();
    uint32_t f, i, v;

    for (f = 0; f < frames; f++) {
        v = fb_rgb(fb->order, (f * 40) & 0xff, (f * 13) & 0xff, 0x80);
        for (i = 0; i < words; i++) {
            base[i] = v;
        }
        mmu_clean_dcache_range(fb->addr, fb->addr + words * 4);
    }
    return systimer_us() - start;
}

void spike_main(uint32_t r0, uint32_t r1, uint32_t r2)
{
    struct board_info board;
    struct fb_info fb;
    struct mmu_status st;
    uint32_t uart_clk = 0;
    uint32_t us;
    int c;

    (void)r0;
    (void)r1;
    (void)r2;

    if (!board_detect(&board)) {
        halt();
    }

    mbox_init(board.periph_base, board.bus_alias);
    if (mbox_get_clock_rate(MBOX_CLOCK_UART, &uart_clk) != 0) {
        uart_clk = UART_FALLBACK_CLOCK;
    }
    uart_init(board.periph_base, uart_clk, board.part != 0xd08);
    systimer_init(board.periph_base);

    k_printf("\n@RC stage=spike3 ev=hello midr=0x%08x soc=%s base=0x%08x\n",
             (unsigned)board.midr, board.soc, (unsigned)board.periph_base);

    if (fb_alloc(board.periph_base, board.bus_alias, FB_W, FB_H, FB_DEPTH, &fb) != 0) {
        k_printf("@RC stage=spike3 ev=fb_error mbox_alias=0x%08x\n", (unsigned)board.bus_alias);
        halt();
    }
    k_printf("@RC stage=spike3 ev=fb raw_addr=0x%08x addr=0x%08x size=%u pitch=%u w=%u h=%u "
             "depth=%u order=%s mbox_alias=0x%08x\n",
             (unsigned)fb.raw_addr, (unsigned)fb.addr, (unsigned)fb.size, (unsigned)fb.pitch,
             (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.depth,
             fb.order == FB_ORDER_RGB ? "rgb" : "bgr", (unsigned)fb.mbox_alias);
    if (fb.depth != 32 || fb.pitch < fb.width * 4) {
        k_printf("@RC stage=spike3 ev=fb_error reason=format\n");
        halt();
    }

    draw_pattern(&fb);

    us = fill_frames(&fb, FILL_FRAMES);
    k_printf("@RC stage=spike3 ev=fill caches=off frames=%u us=%u\n", FILL_FRAMES, (unsigned)us);

    mmu_build_identity(board.periph_base, board.part != 0xb76, fb.addr, fb.size);
    mmu_enable(board.part, &st);
    k_printf("@RC stage=spike3 ev=mmu sctlr=0x%08x smp_reg=%s smp_before=%d smp_write=%s smp_after=%d\n",
             (unsigned)st.sctlr, st.smp_reg, st.smp_before, st.smp_write, st.smp_after);

    us = fill_frames(&fb, FILL_FRAMES);
    k_printf("@RC stage=spike3 ev=fill caches=on frames=%u us=%u sctlr=0x%08x actlr_smp=%d\n",
             FILL_FRAMES, (unsigned)us, (unsigned)st.sctlr, st.smp_after > 0);

    draw_pattern(&fb);
    k_printf("@RC stage=spike3 ev=done\n");
    k_printf("type to echo\n");

    for (;;) {
        c = uart_getc();
        uart_putc(c == '\r' ? '\n' : (char)c);
    }
}
