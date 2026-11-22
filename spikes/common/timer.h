#ifndef SPIKES_TIMER_H
#define SPIKES_TIMER_H

#include <stdint.h>

/* BCM283x system timer, free-running 1 MHz counter. */
void timer_init(uintptr_t periph_base);

uint32_t timer_now_us(void);
void timer_delay_us(uint32_t us);

/* Nonzero once `us` microseconds have passed since `start`. */
int timer_expired(uint32_t start, uint32_t us);

#endif
