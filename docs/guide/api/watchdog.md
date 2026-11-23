# Watchdog

This page covers the hardware watchdog in the power management block. If the main loop stops kicking it, the board does a full reset. The reset returns every GPIO to an input, so loads on relays drop. Return codes and context rules follow the [Conventions](../API.md#conventions).

## Hardware watchdog (`wdog.h`)

`wdog.h` drives the same watchdog that [`power_reboot()`](../API.md#reboot-and-halt-powerh) uses. It needs no init call.

| Constant | Value | Meaning |
|---|---|---|
| `WDOG_TIMEOUT_MIN_MS` | `1` | Shortest timeout |
| `WDOG_TIMEOUT_MAX_MS` | `15999` | Longest timeout |
| `WDOG_ERR_ARG` | `-1` | Timeout outside the range |
| `WDOG_ERR_INIT` | `-2` | Unknown board, so no power management block |
| `WDOG_ERR_STOPPED` | `-3` | `wdog_kick()` with no watchdog started |

The counter in PM_WDOG holds 20 bits of ticks at 65536 per second. The longest count, `0xfffff` ticks, is 15999.98 ms. A timeout in milliseconds rounds up to whole ticks, so the reset never comes before the timeout. One tick is about 15 µs.

```c
int wdog_start(uint32_t timeout_ms);
```

Loads the counter, then sets PM_RSTC for a watchdog full reset. Every write carries the `0x5a` password byte. It keeps the other PM_RSTC bits and does not touch PM_RSTS. Called again, it replaces the timeout. Returns `0`, `WDOG_ERR_ARG` or `WDOG_ERR_INIT`. A refused call leaves a running watchdog as it was. Call it from the main context.

```c
int wdog_kick(void);
```

Reloads the counter with the last timeout and sets the full reset again. It costs two register writes and one read. Call it from the main loop or from an IRQ handler. Kicking from a timer interrupt alone defeats the purpose: the tick keeps running when the main loop hangs. Returns `0`, or `WDOG_ERR_STOPPED` before `wdog_start()` or after `wdog_stop()`.

```c
void wdog_stop(void);
```

Disarms the watchdog. It forgets the timeout first, so a kick from an IRQ handler after that point does nothing. Then it writes `0x5a000102` to PM_RSTC, the value Linux writes to disarm. Calling it twice is harmless. Call it from the main context.

```c
int wdog_running(void);
uint32_t wdog_timeout_ms(void);
uint32_t wdog_remaining_ms(void);
```

`wdog_running()` reads PM_RSTC and returns non-zero when it is set for a watchdog full reset. It reads the hardware, so it also sees a watchdog that an earlier stage left running. `wdog_kick()` cannot reload that one: call `wdog_start()` to take it over. `wdog_timeout_ms()` returns the timeout of the last `wdog_start()`, or `0` when stopped. `wdog_remaining_ms()` reads the counter and rounds down, or returns `0` when stopped. All three are safe in an IRQ handler.

### Reboot, halt and panic

- `power_reboot()` masks IRQs and FIQs, reloads the counter with 10 ticks and resets. A running watchdog does not delay it: no kick can reload the long timeout in between.
- `power_halt()` masks IRQs and FIQs, marks the halt partition in PM_RSTS, then resets the same way. The firmware reads the mark and stays halted. The watchdog is spent by that reset and nothing arms it again, so a halt stays a halt.
- On a panic, the program chooses. Leave the watchdog running and stop kicking it, and the board resets within the timeout. Call `wdog_stop()` first, and the panic screen stays until power is cut. The watchdog counts in hardware, so masked interrupts or a stopped CPU do not stop it.

### Why the last reset cause is not offered

The library offers no function that says "the watchdog fired". `power_reboot()` and an expired watchdog take the same path: a watchdog full reset. So the reset status bits cannot tell "asked for" from "hung". A mark in the partition bits of PM_RSTS would tell them apart, but the firmware reads those bits to choose what to boot, and `0x555` already means halt. `power_reset_status()` still returns the raw PM_RSTS for programs that want to read it.

### What QEMU models

QEMU 10.1's `bcm2835_powermgt` resets the machine at the PM_RSTC write that selects a full reset. It does not count PM_WDOG down. So on `raspi0`, `raspi1ap`, `raspi2b` and `raspi3b`, `wdog_start()` resets at once. A reset sets PM_RSTC to `0x102`, PM_RSTS to `0x1000` and PM_WDOG to `0`. The target test `tests/target/test_wdog.py` checks the argument errors and that an armed watchdog resets the board and the board boots again. The kick, stop, reboot and halt cases skip on QEMU with that reason. The host test `tests/host/test_wdog.c` checks every write against a fake register block.

The tick rate, the counter width, the countdown, the reading of the remaining time and the disarm value come from the BCM2835 peripherals documentation and the Linux `bcm2835_wdt` driver, not from tests on a board. Hardware validation is pending. See [hardware validation](../../HARDWARE_VALIDATION.md).
