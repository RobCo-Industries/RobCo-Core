#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/exc.h>
#include <robco/klibc.h>
#include <robco/mbox.h>
#include <robco/rimg.h> /* compiles the header size checks */
#include <robco/uart.h>

extern robco_boot_args_t robco_boot_args_store;

static robco_board_t board;
static int board_ready;

static void board_detect(void)
{
    uint32_t midr;

    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    board.midr = midr;

    switch ((midr >> 4) & 0xfff) {
    case 0xb76: /* ARM1176JZF-S: Pi Zero, Zero W, 1 */
        board.soc = ROBCO_SOC_BCM2835;
        board.soc_name = "bcm2835";
        board.periph_base = 0x20000000;
        board.bus_alias = 0x40000000;
        break;
    case 0xc07: /* Cortex-A7: Pi 2 v1.1 */
        board.soc = ROBCO_SOC_BCM2836;
        board.soc_name = "bcm2836";
        board.periph_base = 0x3f000000;
        board.bus_alias = 0xc0000000;
        break;
    case 0xd03: /* Cortex-A53: Pi 2 v1.2, 3, 3+, Zero 2 W */
        board.soc = ROBCO_SOC_BCM2837;
        board.soc_name = "bcm2837";
        board.periph_base = 0x3f000000;
        board.bus_alias = 0xc0000000;
        break;
    case 0xd08: /* Cortex-A72: Pi 4, not a target */
        board.soc = ROBCO_SOC_BCM2711;
        board.soc_name = "bcm2711";
        board.periph_base = 0xfe000000;
        board.bus_alias = 0xc0000000;
        break;
    default:
        board.soc = ROBCO_SOC_UNKNOWN;
        board.soc_name = "unknown";
        break;
    }
}

const robco_board_t *board_get(void)
{
    if (!board_ready) {
        board_detect();
        /* Set before the mailbox calls, which need the base. */
        board_ready = 1;
        if (board.periph_base != 0) {
            if (mbox_get_board_rev(&board.board_rev) != 0) {
                board.board_rev = 0;
            }
            if (mbox_get_arm_memory(&board.mem_base, &board.mem_size) != 0) {
                board.mem_base = 0;
                board.mem_size = 0;
            }
        }
    }
    return &board;
}

const robco_boot_args_t *robco_boot_args(void)
{
    return &robco_boot_args_store;
}

void robco_halt(void)
{
    __asm__ volatile("cpsid if");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void robco_start(void)
{
    const robco_board_t *b = board_get();

    switch (b->soc) {
    case ROBCO_SOC_BCM2835:
    case ROBCO_SOC_BCM2836:
    case ROBCO_SOC_BCM2837:
        robco_main();
        break;
    case ROBCO_SOC_BCM2711:
        /* Known base, so report on the PL011. QEMU has no 32-bit raspi4b to test this. */
        uart_init(115200);
        k_printf("\n@RC stage=%s ev=error code=unsupported_board midr=0x%08x\n",
                 ROBCO_STAGE_NAME, (unsigned)b->midr);
        uart_flush();
        break;
    default:
        /* No known base, so no output. */
        break;
    }
    robco_halt();
}
