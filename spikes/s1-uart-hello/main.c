#include <stdint.h>

#include "board.h"
#include "k_printf.h"
#include "mbox.h"
#include "uart_pl011.h"

extern uint32_t boot_entry_cpsr;

void spike_main(uint32_t r0, uint32_t r1, uint32_t r2);

static const char *mode_name(uint32_t cpsr)
{
    switch (cpsr & 0x1f) {
    case 0x10: return "usr";
    case 0x11: return "fiq";
    case 0x12: return "irq";
    case 0x13: return "svc";
    case 0x16: return "mon";
    case 0x17: return "abt";
    case 0x1a: return "hyp";
    case 0x1b: return "und";
    case 0x1f: return "sys";
    default:   return "unknown";
    }
}

void spike_main(uint32_t r0, uint32_t r1, uint32_t r2)
{
    struct board_info board;
    uint32_t board_rev = 0;
    uint32_t mem_base = 0;
    uint32_t mem_size = 0;
    uint32_t uart_clk = 0;
    int clk_from_mbox;
    int c;

    if (!board_detect(&board)) {
        /* Unknown SoC: no safe peripheral base, nothing to print to. */
        for (;;) {
            __asm__ volatile("wfi");
        }
    }

    mbox_init(board.periph_base, board.bus_alias);
    clk_from_mbox = mbox_get_clock_rate(MBOX_CLOCK_UART, &uart_clk) == 0;
    if (!clk_from_mbox) {
        uart_clk = UART_FALLBACK_CLOCK;
    }
    uart_init(board.periph_base, uart_clk, board.part != 0xd08);

    if (mbox_get_board_rev(&board_rev) != 0) {
        board_rev = 0;
    }
    if (mbox_get_arm_memory(&mem_base, &mem_size) != 0) {
        mem_size = 0;
    }

    k_printf("\n@RC stage=spike1 ev=hello midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x uart_clk=%u entry_mode=%s fw_r0=0x%x fw_r1=0x%x fw_r2=0x%x\n",
             (unsigned)board.midr, board.soc, (unsigned)board.periph_base,
             (unsigned)board_rev, (unsigned)mem_size, (unsigned)uart_clk,
             mode_name(boot_entry_cpsr), (unsigned)r0, (unsigned)r1, (unsigned)r2);

    k_printf("RobCo spike 1: UART hello\n");
    if (!board.supported) {
        k_printf("warning: %s is unsupported, UART only\n", board.soc);
    }
    k_printf("arm memory: base 0x%08x size %u MiB\n", (unsigned)mem_base,
             (unsigned)(mem_size >> 20));
    k_printf("uart clock: %u Hz (%s)\n", (unsigned)uart_clk, clk_from_mbox ? "mailbox" : "fallback");
    k_printf("type to echo\n");

    for (;;) {
        c = uart_getc();
        uart_putc(c == '\r' ? '\n' : (char)c);
    }
}
