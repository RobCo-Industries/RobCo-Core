#include <stdint.h>

#include <robco/klibc.h>
#include <robco/power.h>
#include <robco/timer.h>
#include <robco/uart.h>
#include <robco/wdog.h>

#define TIMEOUT_MS  1000u
#define RUN_MS      3000u   /* three timeouts */
#define KICK_MS     100u

/*
 * Prints `ev`, flushes, arms, then prints ev=armed. A board that resets
 * at the arming write, as QEMU does, never prints ev=armed.
 */
static int start(const char *ev, uint32_t ms)
{
    int rc;

    k_printf("@RC stage=wdog ev=%s timeout=%u\n", ev, (unsigned)ms);
    uart_flush();
    rc = wdog_start(ms);
    k_printf("@RC stage=wdog ev=armed rc=%d running=%d\n", rc, wdog_running());
    return rc;
}

/* Arms and never kicks. The board must reset within the timeout. */
static void fire(void)
{
    int rc = start("fire", TIMEOUT_MS);

    timer_delay_ms(RUN_MS);
    k_printf("@RC stage=wdog ev=late rc=%d running=%d\n", rc, wdog_running());
}

/* Kicks for three timeouts, then stops. The board must stay up. */
static void kick(void)
{
    uint32_t t0;
    unsigned n = 0;
    int bad = 0;
    int rc = start("kick", TIMEOUT_MS);

    t0 = timer_now_ms();
    while (timer_now_ms() - t0 < RUN_MS) {
        timer_delay_ms(KICK_MS);
        bad += wdog_kick() != 0;
        n++;
    }
    k_printf("@RC stage=wdog ev=kicked rc=%d n=%u bad=%d running=%d remaining=%u\n", rc, n, bad,
             wdog_running(), (unsigned)wdog_remaining_ms());
    wdog_stop();
    k_printf("@RC stage=wdog ev=stopped running=%d\n", wdog_running());
}

/* Arms, stops at once, then waits three timeouts. The board must stay up. */
static void stop(void)
{
    int rc = start("stop", TIMEOUT_MS);

    wdog_stop();
    timer_delay_ms(RUN_MS);
    k_printf("@RC stage=wdog ev=stayed rc=%d running=%d\n", rc, wdog_running());
}

void robco_main(void)
{
    int c;

    uart_init(115200);
    k_printf("\n@RC stage=wdog ev=boot rsts=0x%08x running=%d\n", (unsigned)power_reset_status(),
             wdog_running());

    k_printf("@RC stage=wdog ev=args zero=%d over=%d kick=%d timeout=%u remaining=%u\n",
             wdog_start(0), wdog_start(WDOG_TIMEOUT_MAX_MS + 1), wdog_kick(),
             (unsigned)wdog_timeout_ms(), (unsigned)wdog_remaining_ms());
    k_printf("@RC stage=wdog ev=ready\n");

    for (;;) {
        c = uart_getc();
        if (c == 'f') {
            fire();
        } else if (c == 'k') {
            kick();
        } else if (c == 's') {
            stop();
        } else if (c == 'r') {
            /* A long timeout, then the library's reboot: it must not wait for it. */
            start("reboot", WDOG_TIMEOUT_MAX_MS);
            uart_flush();
            power_reboot();
        } else if (c == 'h') {
            start("halt", WDOG_TIMEOUT_MAX_MS);
            uart_flush();
            power_halt();
        }
    }
}
