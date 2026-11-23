/*
 * Runs examples/tft's picture through the ILI9341 driver on the fake bus
 * and prints what the example should print: "bytes=<n> crc=<crc32>".
 * tests/target/test_tft.py compares the two.
 */
#include <stdio.h>

#include "tft_fake.h"
#include "../../examples/tft/picture.h"

int main(void)
{
    tft_config_t cfg = TFT_CONFIG_DEFAULT;
    int rc;

    fake_reset(cfg.dc_pin);
    rc = picture_init(&cfg);
    if (rc == 0) {
        rc = picture_draw();
    }
    if (rc != 0) {
        fprintf(stderr, "tft_tool: rc %d\n", rc);
        return 1;
    }
    printf("bytes=%u crc=%08x\n", (unsigned)bus_bytes, (unsigned)bus_crc);
    return 0;
}
