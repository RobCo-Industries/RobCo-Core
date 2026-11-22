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
    console_puts("8x16 cells, Spleen font, four colour schemes.\n");
    console_set_attr(CONSOLE_ATTR_DIM);
    console_puts("Commands: d s t c x 1-4\n");
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
        default:
            continue;
        }
        k_printf("@RC stage=console ev=ok cmd=%c\n", c);
    }
}
