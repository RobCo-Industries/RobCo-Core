#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/font.h>
#include <robco/klibc.h>
#include <robco/uart.h>

static void banner(void)
{
    console_set_attr(CONSOLE_ATTR_BRIGHT);
    console_puts("ROBCO-CORE CONSOLE EXAMPLE\n");
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_puts("8x16 cells, four colour schemes.\n");
    console_set_attr(CONSOLE_ATTR_DIM);
    console_puts("Commands: d s t c x p g 1-4\n");
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_puts("> ");
}

static void scroll_lines(void)
{
    char line[32];
    int i;

    for (i = 1; i <= 40; i++) {
        k_snprintf(line, sizeof(line), "line %02d\n", i);
        console_puts(line);
    }
}

/* Tab stops, backspace, wrap of a 100-character line, then each attribute. */
static void text_test(void)
{
    unsigned col, row;
    int i;

    console_get_cursor(&col, &row);
    if (col != 0) {
        console_putc('\n');
    }
    console_puts("A\tB\tC\n");
    console_puts("abc\bX\n");
    for (i = 0; i < 100; i++) {
        console_putc((char)('0' + i % 10));
    }
    console_putc('\n');
    console_set_attr(CONSOLE_ATTR_DIM);
    console_puts("dim ");
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_puts("normal ");
    console_set_attr(CONSOLE_ATTR_BRIGHT);
    console_puts("bright ");
    console_set_attr(CONSOLE_ATTR_INVERSE);
    console_puts("inverse");
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_putc('\n');
}

static void repeat(int ch, unsigned n)
{
    while (n-- > 0) {
        console_putc((char)ch);
    }
}

/* One row of a frame: left edge, n fill characters, right edge. */
static void frame_row(int left, int fill, int right, unsigned n)
{
    console_putc((char)left);
    repeat(fill, n);
    console_putc((char)right);
}

/* Text inside a double frame, padded to the frame width. */
static void menu_line(const char *text, unsigned width, unsigned attr)
{
    unsigned len = 0;

    console_putc((char)0xba);
    console_set_attr(attr);
    console_puts(text);
    while (text[len] != '\0') {
        len++;
    }
    repeat(' ', width - len);
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_puts("\xba\n");
}

static void attr_word(unsigned attr, const char *word)
{
    console_set_attr(attr);
    console_puts(word);
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_putc(' ');
}

/* A page that shows off the font: boot text, all of ASCII, frames, attributes. */
static void specimen(void)
{
    static const uint8_t shades[] = {0xdb, 0xb2, 0xb1, 0xb0, 0xdf, 0xdc};
    unsigned i;
    int ch;

    console_clear();
    console_set_attr(CONSOLE_ATTR_BRIGHT);
    console_puts("MERIDIAN SYSTEMS BIOS v4.12.03\n");
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_puts("Copyright (C) 1987-1994 Meridian Systems Ltd.\n");
    console_puts("CPU: M6 RISC at 66 MHz, FPU present\n");
    console_puts("Memory test: 65536K OK\n");
    console_set_attr(CONSOLE_ATTR_DIM);
    console_puts("Press F2 for setup, F12 for the boot menu.\n");
    console_set_attr(CONSOLE_ATTR_NORMAL);
    console_puts("Loading shell 3.1 ... done.\n\n");

    for (ch = 0x20; ch < 0x7f; ch++) {
        console_putc((char)ch);
        if (ch == 0x4f) {
            console_putc('\n');
        }
    }
    console_puts("\n");
    for (i = 0; i < sizeof(shades); i++) {
        repeat(shades[i], 4);
        console_putc(' ');
    }
    console_puts("\n\n");

    frame_row(0xc9, 0xcd, 0xbb, 30);
    console_putc('\n');
    menu_line(" MAIN MENU", 30, CONSOLE_ATTR_BRIGHT);
    menu_line("", 30, CONSOLE_ATTR_NORMAL);
    menu_line(" 1. Run diagnostics", 30, CONSOLE_ATTR_INVERSE);
    menu_line(" 2. View system log", 30, CONSOLE_ATTR_NORMAL);
    menu_line(" 3. Configure network", 30, CONSOLE_ATTR_NORMAL);
    menu_line(" 4. Shut down", 30, CONSOLE_ATTR_DIM);
    frame_row(0xc8, 0xcd, 0xbc, 30);
    console_putc('\n');

    /* A small table with every single-line joint. */
    console_putc((char)0xda); repeat(0xc4, 8); console_putc((char)0xc2); repeat(0xc4, 8);
    console_puts("\xbf\n");
    console_puts("\xb3 Disk   \xb3 Free   \xb3\n");
    console_putc((char)0xc3); repeat(0xc4, 8); console_putc((char)0xc5); repeat(0xc4, 8);
    console_puts("\xb4\n");
    console_puts("\xb3 A:     \xb3 1440K  \xb3\n");
    console_putc((char)0xc0); repeat(0xc4, 8); console_putc((char)0xc1); repeat(0xc4, 8);
    console_puts("\xd9\n\n");

    attr_word(CONSOLE_ATTR_DIM, "dim");
    attr_word(CONSOLE_ATTR_NORMAL, "normal");
    attr_word(CONSOLE_ATTR_BRIGHT, "bright");
    attr_word(CONSOLE_ATTR_INVERSE, "inverse");
    console_puts("\n> ");
}

/* Every table entry: ASCII on rows 0 to 1, then bytes 0x80 to 0xff on rows 2 to 3. */
static void glyph_grid(void)
{
    unsigned cols = console_cols();
    int ch;

    console_clear();
    for (ch = 0x20; ch < 0x7f; ch++) {
        console_putc((char)ch);
    }
    console_set_cursor(0, 2);
    for (ch = 0x80; ch <= 0xff; ch++) {
        console_putc((char)ch);
    }
    console_set_cursor(0, 2 + (128 + cols - 1) / cols);
}

static void fill_blocks(void)
{
    unsigned n = console_cols() * console_rows();
    unsigned i;

    console_set_cursor(0, 0);
    for (i = 0; i < n; i++) {
        console_putc((char)FONT_CH_BLOCK);
    }
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    const fb_info_t *fb;
    int rc;
    int c;

    uart_init(115200);
    k_printf("\n@RC stage=console ev=boot soc=%s\n", b->soc_name);
    k_printf("@RC stage=console ev=font name=%s\n", font8x16_name());

    rc = fb_init(640, 480, 32);
    if (rc != 0) {
        k_printf("@RC stage=console ev=error what=fb_init rc=%d\n", rc);
        robco_halt();
    }
    fb = fb_get();
    k_printf("@RC stage=console ev=fb w=%u h=%u pitch=%u order=%s\n",
             (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->pitch,
             fb->order == FB_ORDER_RGB ? "rgb" : "bgr");

    rc = console_init(CONSOLE_SCHEME_GREEN);
    if (rc != 0) {
        k_printf("@RC stage=console ev=error what=console_init rc=%d\n", rc);
        robco_halt();
    }
    console_cursor_show(1);
    banner();
    k_printf("@RC stage=console ev=ready\n");

    for (;;) {
        c = uart_getc();
        switch (c) {
        case 'd':
            console_dump(uart_putc);
            break;
        case '1':
        case '2':
        case '3':
        case '4':
            console_set_scheme((unsigned)(c - '1'));
            break;
        case 's':
            scroll_lines();
            break;
        case 'c':
            console_clear();
            break;
        case 't':
            text_test();
            break;
        case 'x':
            fill_blocks();
            break;
        case 'p':
            specimen();
            break;
        case 'g':
            glyph_grid();
            break;
        default:
            continue;
        }
        k_printf("@RC stage=console ev=ok cmd=%c\n", c);
    }
}
