#include <stdint.h>

#include <robco/board.h>
#include <robco/mmio.h>
#include <robco/power.h>

#define PM_OFFSET           0x100000
#define PM_RSTC             0x1c
#define PM_RSTS             0x20
#define PM_WDOG             0x24

#define PM_PASSWORD         0x5a000000u
#define PM_WDOG_TIME_MASK   0x000fffffu
#define PM_RSTC_WRCFG_CLR   0xffffffcfu
#define PM_RSTC_FULL_RESET  0x00000020u
/* Partition bits are the even bits 0, 2, 4, 6, 8 and 10. */
#define PM_RSTS_PART_MASK   0x00000555u
#define PM_RSTS_HALT        0x00000555u

#define WDOG_TICKS          10

static uint32_t pm_base(void)
{
    const robco_board_t *b = board_get();

    return b->periph_base ? b->periph_base + PM_OFFSET : 0;
}

static void __attribute__((noreturn)) pm_reset(uint32_t pm)
{
    uint32_t rstc;

    if (pm != 0) {
        rstc = mmio_read(pm + PM_RSTC) & PM_RSTC_WRCFG_CLR;
        mmio_write(pm + PM_WDOG, PM_PASSWORD | (WDOG_TICKS & PM_WDOG_TIME_MASK));
        mmio_write(pm + PM_RSTC, PM_PASSWORD | rstc | PM_RSTC_FULL_RESET);
        mmio_dsb();
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void power_reboot(void)
{
    pm_reset(pm_base());
}

void power_halt(void)
{
    uint32_t pm = pm_base();
    uint32_t rsts;

    if (pm != 0) {
        /* Keep the other bits, set the halt partition. Drop the old password byte. */
        rsts = mmio_read(pm + PM_RSTS) & ~(0xff000000u | PM_RSTS_PART_MASK);
        mmio_write(pm + PM_RSTS, PM_PASSWORD | rsts | PM_RSTS_HALT);
    }
    pm_reset(pm);
}

uint32_t power_reset_status(void)
{
    uint32_t pm = pm_base();

    return pm ? mmio_read(pm + PM_RSTS) : 0;
}
