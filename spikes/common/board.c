#include "board.h"

uint32_t board_read_midr(void)
{
    uint32_t midr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    return midr;
}

int board_detect(struct board_info *out)
{
    out->midr = board_read_midr();
    out->part = (out->midr >> 4) & 0xfff;
    out->supported = 1;

    switch (out->part) {
    case 0xb76: /* ARM1176JZF-S: Pi Zero, Zero W, 1 */
        out->soc = "bcm2835";
        out->periph_base = 0x20000000;
        out->bus_alias = 0x40000000;
        return 1;
    case 0xc07: /* Cortex-A7: Pi 2 v1.1 */
        out->soc = "bcm2836";
        out->periph_base = 0x3f000000;
        out->bus_alias = 0xc0000000;
        return 1;
    case 0xd03: /* Cortex-A53: Pi 2 v1.2, 3, 3+, Zero 2 W */
        out->soc = "bcm2837";
        out->periph_base = 0x3f000000;
        out->bus_alias = 0xc0000000;
        return 1;
    case 0xd08: /* Cortex-A72: Pi 4, not a target */
        out->soc = "bcm2711";
        out->periph_base = 0xfe000000;
        out->bus_alias = 0xc0000000;
        out->supported = 0;
        return 1;
    default:
        out->soc = "unknown";
        out->periph_base = 0;
        out->bus_alias = 0;
        out->supported = 0;
        return 0;
    }
}
