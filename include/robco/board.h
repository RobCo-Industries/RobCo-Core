#ifndef ROBCO_BOARD_H
#define ROBCO_BOARD_H

#include <stdint.h>

typedef enum {
    ROBCO_SOC_UNKNOWN = 0,
    ROBCO_SOC_BCM2835 = 0x2835,
    ROBCO_SOC_BCM2836 = 0x2836,
    ROBCO_SOC_BCM2837 = 0x2837,
    ROBCO_SOC_BCM2711 = 0x2711,
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

/* Returned by board_model_name() for a code it does not know. */
#define BOARD_MODEL_UNKNOWN "Raspberry Pi (unknown model)"

/*
 * Model name for a board revision code, such as "Raspberry Pi Zero W".
 * Reads new-style codes (bit 23 set, type in bits 11:4) and the old-style
 * codes of the first boards. Returns BOARD_MODEL_UNKNOWN for 0 and for any
 * code it does not know. Pure: it reads no hardware.
 */
const char *board_model_name(uint32_t revision);

#endif
