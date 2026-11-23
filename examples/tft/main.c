#include <stdint.h>

#include <robco/board.h>
#include <robco/klibc.h>
#include <robco/mmio.h>
#include <robco/tft.h>
#include <robco/timer.h>
#include <robco/uart.h>

#include "picture.h"

#define SPI0_CS 0x204000u

void robco_main(void)
{
    tft_config_t cfg = TFT_CONFIG_DEFAULT;
    uint32_t v = 0;
    uint32_t id = 0;
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=tft ev=boot\n");

    /* A board without the SPI0 block says so and stops, instead of waiting on it. */
    if (mmio_probe(board_get()->periph_base + SPI0_CS, &v) != 0) {
        k_printf("@RC stage=tft ev=nospi\n");
        k_printf("@RC stage=tft ev=done\n");
        for (;;) {
            timer_delay_ms(1000);
        }
    }

    rc = picture_init(&cfg);
    k_printf("@RC stage=tft ev=init rc=%d hz=%u w=%u h=%u\n", rc, (unsigned)tft_get_hz(),
             tft_width(), tft_height());
    if (rc == 0) {
        rc = picture_draw();
        k_printf("@RC stage=tft ev=draw rc=%d bytes=%u crc=%08x\n", rc, (unsigned)bus_bytes,
                 (unsigned)bus_crc);
        tft_set_tap(0);
        rc = tft_read_id(&id);
        k_printf("@RC stage=tft ev=id rc=%d id=%08x\n", rc, (unsigned)id);
    }
    k_printf("@RC stage=tft ev=done\n");
    for (;;) {
        timer_delay_ms(1000);
    }
}
