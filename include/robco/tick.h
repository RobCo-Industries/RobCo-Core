#ifndef ROBCO_TICK_H
#define ROBCO_TICK_H

#include <stdint.h>

#define TICK_MAX_CALLBACKS  4

/*
 * Starts a periodic tick at `hz` on system timer channel 1 (IRQ 1).
 * Needs irq_init() first. IRQs on the CPU stay as they are.
 * Returns 0, or -1 if `hz` is 0, above 10 kHz, or the tick already runs.
 */
int tick_init(uint32_t hz);

/* Ticks since tick_init(). */
uint32_t tick_count(void);

/* Tick count converted to milliseconds. */
uint32_t tick_ms(void);

/* Adds a callback run in IRQ context on every tick. Returns 0 or -1 if full. */
int tick_register(void (*fn)(void));

#endif
