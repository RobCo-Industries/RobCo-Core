#ifndef ROBCO_WDOG_H
#define ROBCO_WDOG_H

#include <stdint.h>

/*
 * Power management watchdog, the same one power_reboot() uses. Once
 * started, the board does a full reset unless wdog_kick() runs before
 * the timeout ends. The reset returns every GPIO to an input.
 *
 * The counter holds 20 bits of 1/65536 s ticks, so the longest timeout
 * is just under 16 s. A timeout in milliseconds rounds up to whole
 * ticks, so the board never resets before `timeout_ms`.
 *
 * wdog_kick(), wdog_running(), wdog_timeout_ms() and wdog_remaining_ms()
 * are safe in an IRQ handler. wdog_start() and wdog_stop() are for the
 * main context.
 */

#define WDOG_TIMEOUT_MIN_MS 1u
#define WDOG_TIMEOUT_MAX_MS 15999u  /* 0xfffff ticks is 15999.98 ms */

#define WDOG_ERR_ARG        (-1)    /* timeout outside the range above */
#define WDOG_ERR_INIT       (-2)    /* unknown board: no PM block */
#define WDOG_ERR_STOPPED    (-3)    /* wdog_kick() with no watchdog running */

/*
 * Arms the watchdog for a full reset after `timeout_ms`. Called again,
 * it replaces the timeout. Returns 0, WDOG_ERR_ARG or WDOG_ERR_INIT.
 */
int wdog_start(uint32_t timeout_ms);

/* Reloads the timeout given to wdog_start(). Returns 0 or WDOG_ERR_STOPPED. */
int wdog_kick(void);

/* Disarms the watchdog. Does nothing when it is not running. */
void wdog_stop(void);

/* Non-zero while the PM block is set for a watchdog full reset. */
int wdog_running(void);

/* Timeout given to the last wdog_start(), or 0 when stopped. */
uint32_t wdog_timeout_ms(void);

/*
 * Time left before the reset, read from the counter and rounded down,
 * or 0 when stopped.
 */
uint32_t wdog_remaining_ms(void);

#endif
