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

#define MODE_MASK 0x1fu

/*
 * Set by exc_set_hook() in src/arch/exc_frame.S. Defined here so a
 * reference to it never pulls that object, and its strong vectors, in.
 */
exc_hook_t robco_exc_hook;

void exc_print_frame(const exc_frame_t *f)
{
    unsigned i;

    k_printf("@RC stage=%s ev=frame", ROBCO_STAGE_NAME);
    for (i = 0; i < 13; i++) {
        k_printf(" r%u=0x%08x", i, (unsigned)f->r[i]);
    }
    k_printf(" sp=0x%08x lr=0x%08x pc=0x%08x cpsr=0x%08x fsr=0x%08x far=0x%08x\n",
             (unsigned)f->sp, (unsigned)f->lr, (unsigned)f->pc, (unsigned)f->cpsr,
             (unsigned)f->fsr, (unsigned)f->far);
}

/* Modes the resume path can restore: their sp and lr bank differs from ABT and UND. */
static int resumable(uint32_t cpsr)
{
    switch (cpsr & MODE_MASK) {
    case 0x10: /* USR */
    case 0x12: /* IRQ */
    case 0x13: /* SVC */
    case 0x1f: /* SYS */
        return 1;
    default:
        return 0;
    }
}

void robco_exc_dispatch(unsigned type, exc_frame_t *frame)
{
    exc_hook_t hook = robco_exc_hook;

    if (hook != 0 && hook(type, frame) == EXC_RESUME && resumable(frame->cpsr)) {
        return;
    }
    robco_exc_report(type, frame->pc, frame->lr, frame->cpsr);
}

/*
 * Default IRQ dispatcher, used when the image does not link the
 * interrupt driver (src/drv/irq_bcm.c). An IRQ is then unexpected.
 */
__attribute__((weak)) void robco_irq_dispatch(uint32_t pc, uint32_t spsr)
{
    robco_exc_report(ROBCO_EXC_IRQ, pc, 0, spsr);
}
