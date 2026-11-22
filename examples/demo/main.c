#include <stdint.h>

#include <robco/alloc.h>
#include <robco/board.h>
#include <robco/boot.h>
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/gpio.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/rng.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define TICK_HZ     100
#define HEAP_SIZE   (1024u * 1024u)
#define LED_PIN     5
#define SECONDS     3

static void fail(const char *what, int rc)
{
    k_printf("@RC stage=demo ev=error what=%s rc=%d\n", what, rc);
    robco_halt();
}

/* The heap has no lock: keep the tick IRQ out while it runs. */
static void *heap_alloc(size_t n)
{
    uint32_t s = irq_save();
    void *p = k_malloc(n);

    irq_restore(s);
    return p;
}

static void heap_read_stats(k_heap_stats_t *st)
{
    uint32_t s = irq_save();

    k_heap_stats(st);
    irq_restore(s);
}

static void status(uint32_t start_ms)
{
    k_heap_stats_t st;
    uint32_t rnd = 0;

    if (rng_u32(&rnd) != 0) {
        fail("rng_u32", 0);
    }
    heap_read_stats(&st);
    k_printf("uptime=%ums ticks=%u used=%u free=%u allocs=%u rng=0x%08x gpio%d=%d\n",
             (unsigned)(timer_now_ms() - start_ms), (unsigned)tick_count(),
             (unsigned)st.used, (unsigned)st.free, (unsigned)st.allocs,
             (unsigned)rnd, LED_PIN, gpio_read(LED_PIN));
}

void robco_main(void)
{
    uint32_t start_ms;
    uint32_t tick_start_ms;
    uint64_t next_us;
    unsigned second;
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=demo ev=boot soc=%s\n", board_get()->soc_name);

    rc = fb_init(640, 480, 32);
    if (rc != 0) {
        fail("fb_init", rc);
    }
    rc = console_init(CONSOLE_SCHEME_GREEN);
    if (rc != 0) {
        fail("console_init", rc);
    }
    console_attach_printf(1);

    irq_init();
    {
        /*
         * The demo runs alone, so it may use the OS heap range above
         * 0x01000000. Its own slot ends at 0x00100000, where the boot info
         * handoff area starts.
         */
        void *base;
        size_t size = robco_heap_high(&base);

        if (size < HEAP_SIZE) {
            fail("heap_region", (int)size);
        }
        k_heap_init(base, HEAP_SIZE);
        k_printf("@RC stage=demo ev=heap base=0x%08x size=%u\n",
                 (unsigned)(uintptr_t)base, (unsigned)HEAP_SIZE);
    }
    rc = rng_init();
    if (rc != 0) {
        fail("rng_init", rc);
    }
    gpio_config_output(LED_PIN, 0);

    tick_start_ms = timer_now_ms();
    rc = tick_init(TICK_HZ);
    if (rc != 0) {
        fail("tick_init", rc);
    }
    irq_cpu_enable();

    k_printf("RobCo-Core demo on %s\n", board_get()->soc_name);
    start_ms = timer_now_ms();
    next_us = timer_now_us();
    for (second = 1; second <= SECONDS; second++) {
        next_us += 1000000;
        while (!timer_expired(next_us)) {
        }
        if (heap_alloc(second * 1000) == NULL) {
            fail("k_malloc", 0);
        }
        gpio_toggle(LED_PIN);
        status(start_ms);
    }

    k_printf("@RC stage=demo ev=done ticks=%u elapsed_ms=%u\n", (unsigned)tick_count(),
             (unsigned)(timer_now_ms() - tick_start_ms));
    console_dump(uart_putc);
    for (;;) {
    }
}
