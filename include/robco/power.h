#ifndef ROBCO_POWER_H
#define ROBCO_POWER_H

#include <stdint.h>

/* Full SoC reset through the PM watchdog. */
void power_reboot(void) __attribute__((noreturn));

/*
 * Marks the reset as a halt (PM_RSTS partition 0x555, the firmware
 * convention), then resets. Waits in wfi if the reset does not happen.
 */
void power_halt(void) __attribute__((noreturn));

/* Raw PM_RSTS. */
uint32_t power_reset_status(void);

#endif
