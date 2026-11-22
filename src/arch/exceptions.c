#include <stdint.h>

#include <robco/boot.h>
#include <robco/exc.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/uart.h>

static const char *const exc_names[] = {
    "reset", "undef", "svc", "prefetch_abort",
    "data_abort", "unused", "irq", "fiq",
};

void robco_exc_report(uint32_t type, uint32_t pc, uint32_t lr, uint32_t spsr)
{
    uint32_t dfsr;
    uint32_t dfar;
    uint32_t ifsr;
    uint32_t clock, baud;

    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
    /*
     * IFSR is the fault status of a prefetch abort, DFSR of a data abort.
     * Short-descriptor status, bits [10] and [3:0], same on ARMv6 (with
     * SCTLR.XP) and ARMv7: 0x5/0x7 translation, 0xd/0xf permission,
     * 0x1 alignment, 0x9/0xb domain.
     */
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));

    /*
     * A fault before uart_init() would be silent. Bring the UART up here.
     * uart_init() does nothing on an unknown board.
     */
    if (uart_get_config(&clock, &baud) != 0) {
        uart_init(115200);
    }

    k_printf("\n@RC stage=%s ev=exception type=%s pc=0x%08x lr=0x%08x spsr=0x%08x "
             "dfsr=0x%08x dfar=0x%08x ifsr=0x%08x\n",
             ROBCO_STAGE_NAME, type < 8 ? exc_names[type] : "?", (unsigned)pc,
             (unsigned)lr, (unsigned)spsr, (unsigned)dfsr, (unsigned)dfar,
             (unsigned)ifsr);
    robco_halt();
}

/*
 * Default IRQ dispatcher, used when the image does not link the
 * interrupt driver (src/drv/irq_bcm.c). An IRQ is then unexpected.
 */
__attribute__((weak)) void robco_irq_dispatch(uint32_t pc, uint32_t spsr)
{
    robco_exc_report(ROBCO_EXC_IRQ, pc, 0, spsr);
}
