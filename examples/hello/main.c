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

#if defined(HELLO_DEAD_TIMER) && HELLO_DEAD_TIMER
#include <robco/audio.h>
#include <robco/irq.h>
#include <robco/tick.h>
#include <robco/timer.h>

/* Reads of the system timer with no change before it counts as stopped. */
#define TIMER_POLLS 1000000u

/*
 * Built with ROBCO_FAULT_TIMER_STOPPED, the library reads a counter that
 * never moves. The image must still print, find the timer stopped, get an
 * error from audio_init() instead of hanging, and reach ev=ready.
 */
static void dead_timer(void)
{
    uint32_t t0 = timer_now_lo();
    uint32_t i;
    int tick;

    k_printf("@RC stage=hello ev=fault_hook name=timer_stopped\n");
    for (i = 0; i < TIMER_POLLS && timer_now_lo() == t0; i++) {
    }
    k_printf("@RC stage=hello ev=timer running=%d\n", i < TIMER_POLLS);
    irq_init();
    tick = tick_init(100);
    irq_cpu_enable();
    k_printf("@RC stage=hello ev=audio tick=%d rc=%d\n", tick, audio_init());
}
#endif

#if defined(HELLO_PRINTF) && HELLO_PRINTF
/*
 * Extreme values for each integer conversion, with width, '-', '0' and a
 * precision above the digit count. tests/target/test_hello.py compares each
 * line. A digit buffer too small for the target shows up here.
 */
static void printf_extremes(void)
{
    k_printf("@RC stage=hello ev=printf_begin\n");
    k_printf("[%u] [%12u] [%-12u] [%012u] [%.12u]\n",
             4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u);
    k_printf("[%d] [%13d] [%-13d] [%013d] [%.12d]\n", (int)(-2147483647 - 1),
             (int)(-2147483647 - 1), (int)(-2147483647 - 1), (int)(-2147483647 - 1),
             (int)(-2147483647 - 1));
    k_printf("[%d] [%12d] [%-12d] [%012d] [%.12d]\n",
             2147483647, 2147483647, 2147483647, 2147483647, 2147483647);
    k_printf("[%x] [%10x] [%-10x] [%010x] [%.10x]\n",
             0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    k_printf("[%X] [%10X] [%-10X] [%010X] [%.10X]\n",
             0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    /* k_printf ignores flags, width and precision for %p; check that too. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    k_printf("[%p] [%12p] [%-12p] [%012p] [%.10p]\n", (void *)0xFFFFFFFCu,
             (void *)0xFFFFFFFCu, (void *)0xFFFFFFFCu, (void *)0xFFFFFFFCu,
             (void *)0xFFFFFFFCu);
#pragma GCC diagnostic pop
    k_printf("@RC stage=hello ev=printf_end\n");
}
#endif

void robco_main(void)
{
    const robco_board_t *b = board_get();
    uint32_t value;
    int pwm;
    int gpio;
    int c;

#if defined(HELLO_EARLY_FAULT) && HELLO_EARLY_FAULT
    /* Faults before uart_init(): the exception report must start the UART itself. */
    __asm__ volatile(".inst 0xe7f000f0");
#endif
    uart_init(115200);

    k_printf("\n@RC stage=hello ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));
#if defined(HELLO_DEAD_TIMER) && HELLO_DEAD_TIMER
    dead_timer();
#endif

    /* PWM aborts on QEMU raspi0/1ap; GPFSEL0 exists everywhere. */
    pwm = mmio_probe(b->periph_base + 0x20c000, &value);
    gpio = mmio_probe(b->periph_base + 0x200000, &value);
    k_printf("@RC stage=hello ev=probe pwm=%s gpio=%s\n",
             pwm ? "abort" : "ok", gpio ? "abort" : "ok");

#if defined(HELLO_PRINTF) && HELLO_PRINTF
    printf_extremes();
#endif
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
