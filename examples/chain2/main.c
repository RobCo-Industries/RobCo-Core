#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define STAGE "chain2"

static robco_bootinfo_t local;
static robco_bootinfo_t scratch;

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case 0x13: return "svc";
    case 0x1a: return "hyp";
    default:   return "other";
    }
}

static uint32_t rd32(uint32_t addr)
{
    return *(volatile const uint32_t *)addr;
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    const robco_bootinfo_t *bi;
    int inherited = bootinfo_inherited();

    uart_init(115200);
    k_printf("\n@RC stage=" STAGE " ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));
    k_printf("@RC stage=" STAGE " ev=handoff trusted=%d\n", inherited);

#if defined(CHAIN2_TICK) && CHAIN2_TICK
    {
        /* No timer match may be left over from the previous stage. */
        int cs1 = timer_compare_pending(1) != 0;
        int cs3 = timer_compare_pending(3) != 0;
        uint64_t end;

        irq_init();
        tick_init(100);
        irq_enable(IRQ_TIMER3);
        irq_cpu_enable();
        end = timer_now_us() + 200000;
        while (!timer_expired(end)) {
        }
        k_printf("@RC stage=" STAGE " ev=tick cs1=%d cs3=%d ticks=%u spurious=%u\n", cs1, cs3,
                 (unsigned)tick_count(), (unsigned)irq_spurious_count());
    }
#endif

    if (!inherited) {
        /* Cold boot: copy the firmware blob and build a fresh block, as a first stage does. */
        bootinfo_copy_fw_blob(ROBCO_HANDOFF_FDT_ADDR, ROBCO_HANDOFF_FDT_MAX);
        bootinfo_build(&local, ROBCO_STAGE_BOOTAGENT);
    }
    bi = bootinfo_get();

    k_printf("@RC stage=" STAGE " ev=bootinfo inherited=%d prev_stage=%u soc=%s mem=0x%08x "
             "fw_r1=0x%08x fdt=0x%08x fdt_size=%u flags=0x%08x\n",
             inherited, (unsigned)bi->stage, b->soc_name, (unsigned)bi->mem_size,
             (unsigned)bi->fw_r1, (unsigned)bi->fdt_addr, (unsigned)bi->fdt_size,
             (unsigned)bi->boot_flags);

    if (bi->fdt_addr == 0 || bi->fdt_size < 8 || rd32(bi->fdt_addr + 4) != ROBCO_ATAG_CORE) {
        k_printf("@RC stage=" STAGE " ev=error code=atags\n");
        uart_flush();
        robco_halt();
    }
    /*
     * This stage sets up no framebuffer or console. The block it builds
     * must still describe the ones the previous stage left running.
     */
    bootinfo_build(&scratch, ROBCO_STAGE_BOOTAGENT);
    k_printf("@RC stage=" STAGE " ev=rebuild id=%u caps=0x%08x fb=0x%08x fb_w=%u fb_h=%u "
             "cols=%u rows=%u cy=%u scheme=%u fdt=0x%08x\n",
             (unsigned)scratch.stage, (unsigned)scratch.caps, (unsigned)scratch.fb_addr,
             (unsigned)scratch.fb_width, (unsigned)scratch.fb_height,
             (unsigned)scratch.con_cols, (unsigned)scratch.con_rows,
             (unsigned)scratch.con_cy, (unsigned)scratch.con_scheme,
             (unsigned)scratch.fdt_addr);
    k_printf("@RC stage=" STAGE " ev=ready\n");
    uart_flush();
}
