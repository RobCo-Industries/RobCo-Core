#ifndef SPIKES_BOARD_H
#define SPIKES_BOARD_H

#include <stdint.h>

struct board_info {
    uint32_t midr;
    uint32_t part;
    const char *soc;
    uintptr_t periph_base;
    uint32_t bus_alias;   /* added to ARM physical addresses given to the VC */
    int supported;        /* 0: run with UART only, if at all */
};

/* Fills `out` from MIDR. Returns 0 when the peripheral base is unknown. */
int board_detect(struct board_info *out);

uint32_t board_read_midr(void);

#endif
