#ifndef SPIKES_SYSTIMER_H
#define SPIKES_SYSTIMER_H

#include <stdint.h>

/* BCM283x system timer: free-running 1 MHz counter at base + 0x3000. */
void systimer_init(uintptr_t periph_base);

/* Low 32 bits of the counter, in microseconds. Wraps after ~71 minutes. */
uint32_t systimer_us(void);

#endif
