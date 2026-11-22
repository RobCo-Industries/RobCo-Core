#ifndef ROBCO_BOARD_H
#define ROBCO_BOARD_H

#include <stdint.h>

typedef enum {
    ROBCO_SOC_UNKNOWN = 0,
    ROBCO_SOC_BCM2835,
    ROBCO_SOC_BCM2836,
    ROBCO_SOC_BCM2837,
    ROBCO_SOC_BCM2711,
} robco_soc_t;

typedef struct {
    uint32_t midr;
    robco_soc_t soc;
    uint32_t periph_base;
    uint32_t bus_alias;     /* ORed into ARM addresses handed to the VC */
    const char *soc_name;
    uint32_t board_rev;     /* 0 if the mailbox did not answer */
    uint32_t mem_base, mem_size;
} robco_board_t;

/* Detects the board on first call, then returns the cached result. */
const robco_board_t *board_get(void);

#endif
