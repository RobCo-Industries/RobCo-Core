#include <stdint.h>

#include <robco/board.h>
#include <robco/irq.h>
#include <robco/mmio.h>

#define IC_OFFSET       0xb200
#define IC_BASIC_PEND   0x00
#define IC_PEND1        0x04
#define IC_PEND2        0x08
#define IC_FIQ_CTRL     0x0c
#define IC_ENABLE1      0x10
#define IC_ENABLE_BASIC 0x18
#define IC_DISABLE1     0x1c
#define IC_DISABLE_BASIC 0x24

/* BCM2836/7 per-core local controller. */
#define LOCAL_GPU_ROUTING 0x4000000c

#define PSR_I           0x80

struct irq_slot {
    irq_handler_t fn;
    void *arg;
};

static struct irq_slot slots[IRQ_COUNT];
/* Shadow of the enable registers: [0] pending1, [1] pending2, [2] basic. */
static volatile uint32_t enabled[3];
static volatile uint32_t spurious;
static uint32_t ic_base;

/* Register offset pair for `irq`: word index into enabled[] and bit. */
static unsigned irq_word(unsigned irq)
{
    return irq < IRQ_GPU_COUNT ? irq / 32 : 2;
}

static uint32_t irq_bit(unsigned irq)
{
    return 1u << (irq < IRQ_GPU_COUNT ? irq % 32 : irq - IRQ_BASIC_FIRST);
}

/* Enable/disable registers are 1, 2, basic: same order as enabled[]. */
static uint32_t irq_reg(unsigned irq, uint32_t first)
{
    unsigned w = irq_word(irq);

    return ic_base + (w == 2 ? first + 8 : first + 4 * w);
}

void irq_init(void)
{
    const robco_board_t *b = board_get();
    unsigned i;

    irq_cpu_disable();
    ic_base = b->periph_base + IC_OFFSET;
    mmio_dmb();
    mmio_write(ic_base + IC_FIQ_CTRL, 0);
    mmio_write(ic_base + IC_DISABLE1, 0xffffffff);
    mmio_write(ic_base + IC_DISABLE1 + 4, 0xffffffff);
    mmio_write(ic_base + IC_DISABLE_BASIC, 0xff);
    for (i = 0; i < 3; i++) {
        enabled[i] = 0;
    }
    for (i = 0; i < IRQ_COUNT; i++) {
        slots[i].fn = 0;
        slots[i].arg = 0;
    }
    spurious = 0;

    /*
     * On BCM2836/7 the legacy controller output reaches a core as its
     * "GPU interrupt" through the local controller. Route it to core 0.
     * BCM2835 has no local controller at that address.
     */
    if (b->soc == ROBCO_SOC_BCM2836 || b->soc == ROBCO_SOC_BCM2837) {
        mmio_write(LOCAL_GPU_ROUTING, 0);
    }
    mmio_dmb();
}

int irq_register(unsigned irq, irq_handler_t fn, void *arg)
{
    uint32_t s;
    int ret = -1;

    if (irq >= IRQ_COUNT || !fn) {
        return -1;
    }
    s = irq_save();
    if (!slots[irq].fn) {
        slots[irq].arg = arg;
        slots[irq].fn = fn;
        ret = 0;
    }
    irq_restore(s);
    return ret;
}

void irq_unregister(unsigned irq)
{
    uint32_t s;

    if (irq >= IRQ_COUNT) {
        return;
    }
    s = irq_save();
    irq_disable(irq);
    slots[irq].fn = 0;
    slots[irq].arg = 0;
    irq_restore(s);
}

void irq_enable(unsigned irq)
{
    uint32_t s;

    if (irq >= IRQ_COUNT || !ic_base) {
        return;
    }
    s = irq_save();
    enabled[irq_word(irq)] |= irq_bit(irq);
    mmio_dmb();
    mmio_write(irq_reg(irq, IC_ENABLE1), irq_bit(irq));
    mmio_dmb();
    irq_restore(s);
}

void irq_disable(unsigned irq)
{
    uint32_t s;

    if (irq >= IRQ_COUNT || !ic_base) {
        return;
    }
    s = irq_save();
    enabled[irq_word(irq)] &= ~irq_bit(irq);
    mmio_dmb();
    mmio_write(irq_reg(irq, IC_DISABLE1), irq_bit(irq));
    mmio_dmb();
    irq_restore(s);
}

uint32_t irq_save(void)
{
    uint32_t cpsr;

    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory");
    return cpsr & PSR_I;
}

void irq_restore(uint32_t state)
{
    if (!(state & PSR_I)) {
        __asm__ volatile("cpsie i" : : : "memory");
    }
}

void irq_cpu_enable(void)
{
    __asm__ volatile("cpsie i" : : : "memory");
}

void irq_cpu_disable(void)
{
    __asm__ volatile("cpsid i" : : : "memory");
}

uint32_t irq_spurious_count(void)
{
    return spurious;
}

/* Runs the handler for one pending source, or disables it if none. */
static void irq_run(unsigned irq)
{
    struct irq_slot *s = &slots[irq];

    if (s->fn) {
        s->fn(irq, s->arg);
    } else {
        spurious++;
        irq_disable(irq);
    }
}

void robco_irq_dispatch(uint32_t pc, uint32_t spsr)
{
    uint32_t pend[3];
    unsigned w;
    unsigned bit;
    unsigned rounds;

    (void)pc;
    (void)spsr;
    if (!ic_base) {
        return;
    }
    /* BCM283x needs a barrier when switching between peripherals. */
    mmio_dmb();
    /*
     * Read pending1/2 directly instead of relying on basic pending bits 8
     * and 9: a few GPU IRQs are only shortcut into basic bits 10..20 and
     * do not set bit 8/9. Masking with the shadow ignores raw lines we
     * never enabled. A bounded number of rounds keeps a source that
     * re-asserts at once from starving the interrupted code forever.
     */
    for (rounds = 0; rounds < 8; rounds++) {
        pend[0] = mmio_read(ic_base + IC_PEND1) & enabled[0];
        pend[1] = mmio_read(ic_base + IC_PEND2) & enabled[1];
        pend[2] = mmio_read(ic_base + IC_BASIC_PEND) & 0xff & enabled[2];
        if (!(pend[0] | pend[1] | pend[2])) {
            break;
        }
        for (w = 0; w < 3; w++) {
            while (pend[w]) {
                bit = (unsigned)__builtin_ctz(pend[w]);
                pend[w] &= pend[w] - 1;
                irq_run(w == 2 ? IRQ_BASIC_FIRST + bit : w * 32 + bit);
                mmio_dmb();
            }
        }
    }
    mmio_dmb();
}
