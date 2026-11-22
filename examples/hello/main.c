#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/klibc.h>
#include <robco/mmio.h>
#include <robco/uart.h>

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
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

void robco_main(void)
{
    const robco_board_t *b = board_get();
    uint32_t value;
    int pwm;
    int gpio;
    int c;

    uart_init(115200);

    k_printf("\n@RC stage=hello ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));

    /* PWM aborts on QEMU raspi0/1ap; GPFSEL0 exists everywhere. */
    pwm = mmio_probe(b->periph_base + 0x20c000, &value);
    gpio = mmio_probe(b->periph_base + 0x200000, &value);
    k_printf("@RC stage=hello ev=probe pwm=%s gpio=%s\n",
             pwm ? "abort" : "ok", gpio ? "abort" : "ok");

    k_printf("@RC stage=hello ev=ready\n");

#if defined(HELLO_FAULT) && HELLO_FAULT
    /* Permanently undefined encoding (UDF #0); must reach the default handler. */
    __asm__ volatile(".inst 0xe7f000f0");
#endif

    for (;;) {
        c = uart_getc();
        uart_putc(c == '\r' ? '\n' : (char)c);
    }
}
