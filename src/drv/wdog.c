#include <stdint.h>

#include <robco/board.h>
#include <robco/mmio.h>
#include <robco/wdog.h>

/* Same PM block and register layout as power.c. */
#define PM_OFFSET           0x100000
#define PM_RSTC             0x1c
#define PM_WDOG             0x24

#define PM_PASSWORD         0x5a000000u
#define PM_WDOG_TIME_MASK   0x000fffffu
#define PM_RSTC_WRCFG_MASK  0x00000030u
#define PM_RSTC_WRCFG_CLR   0xffffffcfu
#define PM_RSTC_FULL_RESET  0x00000020u
/* Value Linux writes to disarm: no watchdog reset configured. */
#define PM_RSTC_RESET       0x00000102u

#define TICKS_PER_S         65536u

/* Ticks for the running timeout, or 0 when stopped. Read by wdog_kick() in IRQs. */
static volatile uint32_t ticks;
static volatile uint32_t timeout;

static uint32_t pm_base(void)
{
    const robco_board_t *b = board_get();

    return b->periph_base ? b->periph_base + PM_OFFSET : 0;
}

/* Loads the counter, then selects the full reset. Linux's bcm2835_wdt does the same. */
static void arm(uint32_t pm, uint32_t t)
{
    uint32_t rstc = mmio_read(pm + PM_RSTC) & PM_RSTC_WRCFG_CLR;

    mmio_write(pm + PM_WDOG, PM_PASSWORD | (t & PM_WDOG_TIME_MASK));
    mmio_write(pm + PM_RSTC, PM_PASSWORD | rstc | PM_RSTC_FULL_RESET);
    mmio_dsb();
}

int wdog_start(uint32_t timeout_ms)
{
    uint32_t pm = pm_base();
    uint32_t t;

    if (timeout_ms < WDOG_TIMEOUT_MIN_MS || timeout_ms > WDOG_TIMEOUT_MAX_MS) {
        return WDOG_ERR_ARG;
    }
    if (pm == 0) {
        return WDOG_ERR_INIT;
    }
    /* Round up: the reset never comes before timeout_ms. */
    t = (uint32_t)(((uint64_t)timeout_ms * TICKS_PER_S + 999u) / 1000u);
    ticks = t;
    timeout = timeout_ms;
    arm(pm, t);
    return 0;
}

int wdog_kick(void)
{
    uint32_t t = ticks;
    uint32_t pm;

    if (t == 0) {
        return WDOG_ERR_STOPPED;
    }
    pm = pm_base();
    arm(pm, t);
    return 0;
}

void wdog_stop(void)
{
    uint32_t pm = pm_base();

    /* Clear first: a kick from an IRQ after this point does nothing. */
    ticks = 0;
    timeout = 0;
    if (pm != 0) {
        mmio_write(pm + PM_RSTC, PM_PASSWORD | PM_RSTC_RESET);
        mmio_dsb();
    }
}

int wdog_running(void)
{
    uint32_t pm = pm_base();

    if (pm == 0) {
        return 0;
    }
    return (mmio_read(pm + PM_RSTC) & PM_RSTC_WRCFG_MASK) == PM_RSTC_FULL_RESET;
}

uint32_t wdog_timeout_ms(void)
{
    return timeout;
}

uint32_t wdog_remaining_ms(void)
{
    uint32_t pm = pm_base();
    uint32_t left;

    if (pm == 0 || ticks == 0) {
        return 0;
    }
    left = mmio_read(pm + PM_WDOG) & PM_WDOG_TIME_MASK;
    return (uint32_t)((uint64_t)left * 1000u / TICKS_PER_S);
}
