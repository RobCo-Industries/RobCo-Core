#ifndef ROBCO_CONSOLE_H
#define ROBCO_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* Cell buffer limits. robco_config.h may raise or lower them. */
#ifndef ROBCO_CONSOLE_MAX_COLS
#define ROBCO_CONSOLE_MAX_COLS 100
#endif
#ifndef ROBCO_CONSOLE_MAX_ROWS
#define ROBCO_CONSOLE_MAX_ROWS 40
#endif

/* Scheme ids. The boot info con_scheme field uses the same numbers. */
#define CONSOLE_SCHEME_GREEN    0
#define CONSOLE_SCHEME_AMBER    1
#define CONSOLE_SCHEME_BLUE     2
#define CONSOLE_SCHEME_WHITE    3
#define CONSOLE_SCHEME_COUNT    4

/* Brightness is one of the first three. INVERSE can be ORed in. */
#define CONSOLE_ATTR_NORMAL     0
#define CONSOLE_ATTR_DIM        1
#define CONSOLE_ATTR_BRIGHT     2
#define CONSOLE_ATTR_INVERSE    4

#define CONSOLE_ERR_NO_FB       (-1)
#define CONSOLE_ERR_TOO_BIG     (-2)
#define CONSOLE_ERR_SCHEME      (-3)
#define CONSOLE_ERR_CURSOR      (-4)

/* console_dump() shows a cell whose character is unknown, after console_adopt(), as this. */
#define CONSOLE_DUMP_UNKNOWN    '#'

typedef struct {
    uint8_t r, g, b;
} console_rgb_t;

typedef struct {
    const char *name;
    console_rgb_t bg, dim, normal, bright;
} console_scheme_t;

/* NULL for an unknown id. Defined in schemes.c. */
const console_scheme_t *console_scheme_info(unsigned scheme);

/*
 * Needs fb_init first. Clears the grid and resets the grid, cursor and
 * attribute. Call it again after every successful fb_init(): until then
 * the console draws nothing and console_cols() returns 0.
 */
int console_init(unsigned scheme);

/*
 * Starts the console on the framebuffer an earlier stage left on, after
 * fb_adopt(), without clearing it. The grid is `cols` by `rows` cells,
 * the cursor at `cx`,`cy`, hidden, attribute normal. Checks, in order: a
 * framebuffer (CONSOLE_ERR_NO_FB), the scheme (CONSOLE_ERR_SCHEME), a
 * non-empty grid that fits the screen and the cell buffer
 * (CONSOLE_ERR_TOO_BIG), the cursor inside the grid (CONSOLE_ERR_CURSOR).
 * On an error nothing changes, on screen or in the console state.
 *
 * The characters already on screen are unknown: console_dump() shows
 * them as CONSOLE_DUMP_UNKNOWN until they are written over. Scrolling
 * moves pixels, so they move up intact. The console draws, clears and
 * scrolls only inside its grid: pixels right of it and below it stay. console_set_scheme() repaints
 * only known cells, and the block cursor does not show on an unknown
 * cell.
 */
int console_adopt(unsigned cols, unsigned rows, unsigned cx, unsigned cy, unsigned scheme);

/*
 * Handles '\n' (new line, column 0), '\r', '\b' (moves left, no erase)
 * and '\t' (next multiple of 8). A character written after one in the
 * last column goes to the next row, and the bottom row scrolls up.
 * Bytes 0x80 to 0xff draw the CP437 glyphs that
 * font.h lists. Other control characters are ignored.
 */
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_puts(const char *s);

void console_clear(void);
void console_set_cursor(unsigned col, unsigned row);
void console_get_cursor(unsigned *col, unsigned *row);
/* 0 before console_init() and while the console is stale. */
unsigned console_cols(void);
unsigned console_rows(void);
/* Block cursor drawn by inverting the cell under it. Off after init. */
void console_cursor_show(int on);

/*
 * Applies to characters written after the call. `attr` is a brightness,
 * optionally ORed with CONSOLE_ATTR_INVERSE. Any other value is ignored
 * and the attribute stays as it was.
 */
void console_set_attr(unsigned attr);
/*
 * Redraws the whole grid from the cell buffer. After console_adopt(),
 * cells still unknown keep their pixels. Unknown ids are ignored.
 */
void console_set_scheme(unsigned scheme);
/* The active scheme id, CONSOLE_SCHEME_GREEN before console_init(). */
unsigned console_scheme(void);

/*
 * Test hook. Writes an "@RC ... ev=screen" header, one "|" line per row
 * with trailing spaces trimmed, then "@RC ... ev=screen_end". Cells
 * outside printable ASCII, and unknown cells after console_adopt(), show
 * as '#'.
 */
void console_dump(void (*out)(char));

/* Routes k_printf to the console, and also to the UART if also_uart. */
void console_attach_printf(int also_uart);

#endif
