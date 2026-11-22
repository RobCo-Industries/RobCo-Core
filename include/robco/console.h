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

typedef struct {
    uint8_t r, g, b;
} console_rgb_t;

typedef struct {
    const char *name;
    console_rgb_t bg, dim, normal, bright;
} console_scheme_t;

/* NULL for an unknown id. Defined in schemes.c. */
const console_scheme_t *console_scheme(unsigned scheme);

/* Needs fb_init first. Clears the screen. Returns 0 or CONSOLE_ERR_*. */
int console_init(unsigned scheme);

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
unsigned console_cols(void);
unsigned console_rows(void);
/* Block cursor drawn by inverting the cell under it. Off after init. */
void console_cursor_show(int on);

/* Applies to characters written after the call. */
void console_set_attr(unsigned attr);
/* Redraws the whole screen from the cell buffer. Unknown ids are ignored. */
void console_set_scheme(unsigned scheme);

/*
 * Test hook. Writes an "@RC ... ev=screen" header, one "|" line per row
 * with trailing spaces trimmed, then "@RC ... ev=screen_end". Cells
 * outside printable ASCII show as '#'.
 */
void console_dump(void (*out)(char));

/* Routes k_printf to the console, and also to the UART if also_uart. */
void console_attach_printf(int also_uart);

#endif
