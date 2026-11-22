#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/klibc.h>
#include <robco/power.h>
#include <robco/rng.h>
#include <robco/uart.h>

static void print_out(void)
{
    k_printf("@RC stage=gpio ev=out pin5=%d pin6=%d fsel5=%d fsel6=%d\n",
             gpio_read(5), gpio_read(6), gpio_get_function(5), gpio_get_function(6));
}

/* Returns 1 if every API rejects pin 54. */
static int bad_pin_rejected(void)
{
    return gpio_set_function(54, GPIO_FN_OUTPUT) < 0
        && gpio_get_function(54) < 0
        && gpio_set_pull(54, GPIO_PULL_NONE) < 0
        && gpio_write(54, 1) < 0
        && gpio_read(54) < 0
        && gpio_toggle(54) < 0
        && gpio_config_output(54, 0) < 0
        && gpio_config_input(54, GPIO_PULL_NONE) < 0;
}

void robco_main(void)
{
    uint32_t a;
    uint32_t b;
    int rc;
    int c;

    uart_init(115200);

    k_printf("\n@RC stage=gpio ev=boot soc=%s rsts=0x%08x\n",
             board_get()->soc_name, (unsigned)power_reset_status());

    gpio_config_output(5, 1);
    gpio_config_output(6, 0);
    print_out();
    gpio_toggle(5);
    gpio_toggle(6);
    print_out();

    gpio_config_input(17, GPIO_PULL_UP);
    k_printf("@RC stage=gpio ev=in pin17=%d fsel17=%d\n", gpio_read(17), gpio_get_function(17));

    k_printf("@RC stage=gpio ev=uartpins fsel14=%d fsel15=%d\n",
             gpio_get_function(14), gpio_get_function(15));

    k_printf("@RC stage=gpio ev=badpin rc=%d\n", bad_pin_rejected());

    rc = rng_init();
    a = b = 0;
    if (rc == 0) {
        rc = rng_u32(&a);
    }
    if (rc == 0) {
        rc = rng_u32(&b);
    }
    k_printf("@RC stage=gpio ev=rng a=0x%08x b=0x%08x rc=%d\n", (unsigned)a, (unsigned)b, rc);

    k_printf("@RC stage=gpio ev=ready\n");

    for (;;) {
        c = uart_getc();
        if (c == 'r') {
            k_printf("@RC stage=gpio ev=reboot\n");
            uart_flush();
            power_reboot();
        } else if (c == 'h') {
            k_printf("@RC stage=gpio ev=halt\n");
            uart_flush();
            power_halt();
        }
    }
}
