#ifndef SPIKES_MBOX_H
#define SPIKES_MBOX_H

#include <stdint.h>

#define MBOX_TAG_BOARD_REV   0x00010002
#define MBOX_TAG_ARM_MEMORY  0x00010005
#define MBOX_TAG_CLOCK_RATE  0x00030002

#define MBOX_CLOCK_UART      2

void mbox_init(uintptr_t periph_base, uint32_t bus_alias);

/* Each returns 0 on success, -1 on failure. */
int mbox_get_board_rev(uint32_t *rev);
int mbox_get_arm_memory(uint32_t *base, uint32_t *size);
int mbox_get_clock_rate(uint32_t clock_id, uint32_t *rate);

#endif
