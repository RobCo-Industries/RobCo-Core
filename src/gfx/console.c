#include <stddef.h>
#include <stdint.h>

#include <robco/console.h>
#include <robco/exc.h>
#include <robco/fb.h>
#include <robco/font.h>
#include <robco/klibc.h>
#include <robco/uart.h>

#define TAB_WIDTH   8
#define ATTR_LEVEL  3u

typedef struct {
    uint8_t ch;
    uint8_t attr;
} cell_t;

static cell_t cells[ROBCO_CONSOLE_MAX_ROWS][ROBCO_CONSOLE_MAX_COLS];
static unsigned cols, rows;
static unsigned cx, cy;
static unsigned cur_attr;
static unsigned cur_scheme;
static int cursor_on;
/* Set after a write to the last column: the next printable char wraps. */
static int wrap_pending;
static int ready;
/* Pixel words for the active scheme: background, dim, normal, bright. */
static uint32_t px_bg, px_level[3];

static void load_scheme(unsigned scheme)
{
    const console_scheme_t *s = console_scheme_info(scheme);

    cur_scheme = scheme;
    px_bg = fb_rgb(s->bg.r, s->bg.g, s->bg.b);
    px_level[CONSOLE_ATTR_NORMAL] = fb_rgb(s->normal.r, s->normal.g, s->normal.b);
    px_level[CONSOLE_ATTR_DIM] = fb_rgb(s->dim.r, s->dim.g, s->dim.b);
    px_level[CONSOLE_ATTR_BRIGHT] = fb_rgb(s->bright.r, s->bright.g, s->bright.b);
}

/* Draws one cell. `invert` swaps foreground and background again. */
static void draw_cell(unsigned col, unsigned row, int invert)
{
    const cell_t *c = &cells[row][col];
    unsigned level = c->attr & ATTR_LEVEL;
    uint32_t fg, bg, t;

    fg = px_level[level < 3 ? level : CONSOLE_ATTR_NORMAL];
    bg = px_bg;
    if (((c->attr & CONSOLE_ATTR_INVERSE) != 0) != (invert != 0)) {
        t = fg;
        fg = bg;
        bg = t;
    }
    fb_blit_1bpp(col * FONT_W, row * FONT_H, FONT_W, FONT_H,
                 font8x16_glyph(c->ch), fg, bg);
}

static void cursor_hide_raw(void)
{
    if (cursor_on && cx < cols && cy < rows) {
        draw_cell(cx, cy, 0);
    }
}

static void cursor_draw_raw(void)
{
    if (cursor_on && cx < cols && cy < rows) {
        draw_cell(cx, cy, 1);
    }
}

static void clear_cells(void)
{
    unsigned r, c;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            cells[r][c].ch = ' ';
            cells[r][c].attr = CONSOLE_ATTR_NORMAL;
        }
    }
}

static void redraw_all(void)
{
    unsigned r, c;

    fb_clear(px_bg);
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            if (cells[r][c].ch != ' ' || cells[r][c].attr != CONSOLE_ATTR_NORMAL) {
                draw_cell(c, r, 0);
            }
        }
    }
}

static void scroll(void)
{
    unsigned c;

    k_memmove(&cells[0][0], &cells[1][0], sizeof(cells[0]) * (rows - 1));
    for (c = 0; c < cols; c++) {
        cells[rows - 1][c].ch = ' ';
        cells[rows - 1][c].attr = CONSOLE_ATTR_NORMAL;
    }
    fb_scroll_up(FONT_H, px_bg);
    /* Rows below rows * FONT_H, if any, stay background. */
}

static void newline(void)
{
    wrap_pending = 0;
    cx = 0;
    if (cy + 1 < rows) {
        cy++;
    } else {
        scroll();
    }
}

static void put_raw(char ch)
{
    uint8_t c = (uint8_t)ch;

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        wrap_pending = 0;
        cx = 0;
        return;
    case '\b':
        if (wrap_pending) {
            wrap_pending = 0;
        } else if (cx > 0) {
            cx--;
        }
        return;
    case '\t':
        wrap_pending = 0;
        cx = (cx / TAB_WIDTH + 1) * TAB_WIDTH;
        if (cx >= cols) {
            newline();
        }
        return;
    default:
        break;
    }
    if (c < 0x20 || c == 0x7f) {
        return;
    }
    if (wrap_pending) {
        newline();
    }
    cells[cy][cx].ch = c;
    cells[cy][cx].attr = (uint8_t)cur_attr;
    draw_cell(cx, cy, 0);
    if (cx + 1 < cols) {
        cx++;
    } else {
        wrap_pending = 1;
    }
}

int console_init(unsigned scheme)
{
    const fb_info_t *fb = fb_get();
    unsigned c, r;

    if (fb == NULL) {
        return CONSOLE_ERR_NO_FB;
    }
    if (console_scheme_info(scheme) == NULL) {
        return CONSOLE_ERR_SCHEME;
    }
    c = fb->width / FONT_W;
    r = fb->height / FONT_H;
    if (c == 0 || r == 0 || c > ROBCO_CONSOLE_MAX_COLS || r > ROBCO_CONSOLE_MAX_ROWS) {
        return CONSOLE_ERR_TOO_BIG;
    }
    cols = c;
    rows = r;
    cx = cy = 0;
    wrap_pending = 0;
    cur_attr = CONSOLE_ATTR_NORMAL;
    cursor_on = 0;
    load_scheme(scheme);
    clear_cells();
    fb_clear(px_bg);
    ready = 1;
    return 0;
}

void console_putc(char c)
{
    if (!ready) {
        return;
    }
    cursor_hide_raw();
    put_raw(c);
    cursor_draw_raw();
}

void console_write(const char *s, size_t n)
{
    size_t i;

    if (!ready) {
        return;
    }
    cursor_hide_raw();
    for (i = 0; i < n; i++) {
        put_raw(s[i]);
    }
    cursor_draw_raw();
}

void console_puts(const char *s)
{
    console_write(s, k_strlen(s));
}

void console_clear(void)
{
    if (!ready) {
        return;
    }
    clear_cells();
    cx = cy = 0;
    wrap_pending = 0;
    fb_clear(px_bg);
    cursor_draw_raw();
}

void console_set_cursor(unsigned col, unsigned row)
{
    if (!ready) {
        return;
    }
    cursor_hide_raw();
    wrap_pending = 0;
    cx = col < cols ? col : cols - 1;
    cy = row < rows ? row : rows - 1;
    cursor_draw_raw();
}

void console_get_cursor(unsigned *col, unsigned *row)
{
    if (col != NULL) {
        *col = cx;
    }
    if (row != NULL) {
        *row = cy;
    }
}

unsigned console_cols(void)
{
    return cols;
}

unsigned console_rows(void)
{
    return rows;
}

void console_cursor_show(int on)
{
    if (!ready) {
        return;
    }
    cursor_hide_raw();
    cursor_on = on != 0;
    cursor_draw_raw();
}

void console_set_attr(unsigned attr)
{
    cur_attr = attr & (ATTR_LEVEL | CONSOLE_ATTR_INVERSE);
}

unsigned console_scheme(void)
{
    return cur_scheme;
}

void console_set_scheme(unsigned scheme)
{
    if (!ready || console_scheme_info(scheme) == NULL) {
        return;
    }
    load_scheme(scheme);
    redraw_all();
    cursor_draw_raw();
}

static void out_str(void (*out)(char), const char *s)
{
    while (*s) {
        out(*s++);
    }
}

void console_dump(void (*out)(char))
{
    char buf[96];
    unsigned r, c, end;
    uint8_t ch;

    k_snprintf(buf, sizeof(buf),
               "@RC stage=%s ev=screen cols=%u rows=%u cx=%u cy=%u scheme=%s\n",
               ROBCO_STAGE_NAME, cols, rows, cx, cy,
               ready ? console_scheme_info(cur_scheme)->name : "none");
    out_str(out, buf);
    for (r = 0; r < rows; r++) {
        end = cols;
        while (end > 0 && cells[r][end - 1].ch == ' ') {
            end--;
        }
        out('|');
        for (c = 0; c < end; c++) {
            ch = cells[r][c].ch;
            out(ch >= 0x20 && ch < 0x7f ? (char)ch : '#');
        }
        out('\n');
    }
    k_snprintf(buf, sizeof(buf), "@RC stage=%s ev=screen_end\n", ROBCO_STAGE_NAME);
    out_str(out, buf);
}

static void putc_both(char c)
{
    console_putc(c);
    uart_putc(c);
}

void console_attach_printf(int also_uart)
{
    k_console_set(also_uart ? putc_both : console_putc);
}
