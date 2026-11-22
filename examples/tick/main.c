#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define TICK_HZ         100
#define DONE_TICKS      200
#define ONESHOT_US      5000
#define ONESHOT_CH      3

/* Tick timestamps, indexed by tick number modulo the ring size. */
#define RING            16
static volatile uint32_t tick_us[RING];
static volatile uint32_t done_us;

static volatile uint32_t oneshot_deadline;
static volatile uint32_t oneshot_late;
static volatile int oneshot_fired;

#ifdef TICK_SLOW
/*
 * Slow variant: the callback takes longer than one tick period. The tick
 * handler must still return, so the main loop keeps running.
 */
#define SLOW_CB_US      15000
#define SLOW_TICKS      100

static volatile uint32_t slow_calls;

/* IRQ context: busy for longer than a whole period. */
static void on_slow_tick(void)
{
    slow_calls++;
    timer_delay_us(SLOW_CB_US);
}

static void slow_main(void)
{
    uint32_t loops = 0;
    uint32_t start_us;
    int done = 0;
    int c;

    tick_register(on_slow_tick);
    start_us = (uint32_t)timer_now_us();
    tick_init(TICK_HZ);
    irq_cpu_enable();
    for (;;) {
        loops++;
        if (!done && tick_count() >= SLOW_TICKS) {
            done = 1;
            k_printf("@RC stage=tick ev=slow ticks=%u calls=%u missed=%u ms=%u "
                     "elapsed_us=%u loops=%u\n",
                     (unsigned)tick_count(), (unsigned)slow_calls,
                     (unsigned)tick_missed(), (unsigned)tick_ms(),
                     (unsigned)((uint32_t)timer_now_us() - start_us), (unsigned)loops);
        }
        c = uart_poll();
        if (c >= 0) {
            uart_putc(c == '\r' ? '\n' : (char)c);
        }
    }
}
#endif

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

/* IRQ context: record when each tick ran. */
static void on_tick(void)
{
    uint32_t n = tick_count();
    uint32_t now = (uint32_t)timer_now_us();

    tick_us[n % RING] = now;
    if (n == DONE_TICKS) {
        done_us = now;
    }
}

/* IRQ context: channel 3 one-shot. Measure lateness, then switch it off. */
static void on_oneshot(unsigned irq, void *arg)
{
    (void)arg;
    oneshot_late = timer_now_lo() - oneshot_deadline;
    timer_compare_clear(ONESHOT_CH);
    irq_disable(irq);
    oneshot_fired = 1;
}

static int report_tick(uint32_t n)
{
    return n <= 5 || n % 50 == 0;
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    uint32_t seen = 0;
    uint32_t start_us;
    uint64_t next_alive;
    int done = 0;
    int oneshot_done = 0;
    uint32_t n;
    int c;

    uart_init(115200);

    k_printf("\n@RC stage=tick ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));

    irq_init();
#ifdef TICK_SLOW
    slow_main();
#endif
    tick_register(on_tick);
    irq_register(IRQ_TIMER3, on_oneshot, 0);

    start_us = (uint32_t)timer_now_us();
    tick_init(TICK_HZ);
    oneshot_deadline = timer_now_lo() + ONESHOT_US;
    timer_compare_set(ONESHOT_CH, oneshot_deadline);
    timer_compare_clear(ONESHOT_CH);
    irq_enable(IRQ_TIMER3);
    irq_cpu_enable();

    next_alive = 0;
    for (;;) {
        n = tick_count();
        while (seen < n) {
            seen++;
            if (report_tick(seen)) {
                k_printf("@RC stage=tick ev=tick n=%u us=%u\n",
                         (unsigned)seen, (unsigned)tick_us[seen % RING]);
            }
        }
        if (!oneshot_done && oneshot_fired) {
            oneshot_done = 1;
            k_printf("@RC stage=tick ev=oneshot late_us=%u\n", (unsigned)oneshot_late);
        }
        if (!done && seen >= DONE_TICKS) {
            done = 1;
            k_printf("@RC stage=tick ev=done ticks=%u elapsed_us=%u spurious=%u\n",
                     (unsigned)DONE_TICKS, (unsigned)(done_us - start_us),
                     (unsigned)irq_spurious_count());
            next_alive = timer_now_us() + 1000000;
        }
        if (done && timer_expired(next_alive)) {
            next_alive += 1000000;
            k_printf("@RC stage=tick ev=alive ticks=%u\n", (unsigned)tick_count());
        }
        c = uart_poll();
        if (c >= 0) {
            uart_putc(c == '\r' ? '\n' : (char)c);
        }
    }
}
