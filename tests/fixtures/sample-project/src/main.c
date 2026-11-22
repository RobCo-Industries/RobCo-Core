#include <robco/board.h>
#include <robco/klibc.h>
#include <robco/uart.h>

#include "greet.h"

void robco_main(void)
{
    char buf[32];

    uart_init(115200);
    greet(buf, sizeof(buf), board_get()->soc_name, 1);
    k_printf("\n@RC stage=sample ev=ready text=%s\n", buf);
    for (;;) {
        uart_putc((char)uart_getc());
    }
}
