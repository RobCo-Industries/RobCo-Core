#include <stdint.h>

#include "board.h"
#include "crc32.h"
#include "emmc.h"
#include "gpio.h"
#include "k_printf.h"
#include "mbox.h"
#include "timer.h"
#include "uart_pl011.h"

#define MBOX_CLOCK_EMMC 1

void spike_main(uint32_t r0, uint32_t r1, uint32_t r2);

static uint32_t sector[EMMC_BLOCK_SIZE / 4];

static void print_fsel(uint32_t first, uint32_t last)
{
    uint32_t pin;

    k_printf("@RC stage=spike2 ev=gpio pins=%u-%u fsel=", (unsigned)first, (unsigned)last);
    for (pin = first; pin <= last; pin++) {
        k_printf("%u", (unsigned)gpio_get_fsel(pin));
    }
    k_printf("\n");
}

static int pins_any(uint32_t first, uint32_t last, uint32_t fsel)
{
    uint32_t pin;

    for (pin = first; pin <= last; pin++) {
        if (gpio_get_fsel(pin) == fsel) {
            return 1;
        }
    }
    return 0;
}

static int pins_all(uint32_t first, uint32_t last, uint32_t fsel)
{
    uint32_t pin;

    for (pin = first; pin <= last; pin++) {
        if (gpio_get_fsel(pin) != fsel) {
            return 0;
        }
    }
    return 1;
}

/*
 * Moves the SD card to the EMMC controller. The two pin groups are handled
 * independently: GPIO 34-39 go to input if any is ALT3 (WiFi side), GPIO
 * 48-53 go to ALT3 if any is not ALT3. Never selects ALT0. Returns 1 if a
 * pin changed.
 */
static int remux_sd_to_emmc(void)
{
    uint32_t pin;
    int low = 0;
    int high = 0;

    if (pins_any(34, 39, GPIO_FSEL_ALT3)) {
        for (pin = 34; pin <= 39; pin++) {
            gpio_set_fsel(pin, GPIO_FSEL_IN);
        }
        low = 1;
    }
    if (!pins_all(48, 53, GPIO_FSEL_ALT3)) {
        for (pin = 48; pin <= 53; pin++) {
            gpio_set_fsel(pin, GPIO_FSEL_ALT3);
        }
        high = 1;
    }
    k_printf("@RC stage=spike2 ev=remux pins34_39=%s pins48_53=%s\n", low ? "changed" : "kept",
             high ? "changed" : "kept");
    timer_delay_us(1000);
    return low || high;
}

static void print_error(const struct emmc_error *err)
{
    k_printf("@RC stage=spike2 ev=error step=%s status=0x%08x interrupt=0x%08x\n", err->step,
             (unsigned)err->status, (unsigned)err->interrupt);
}

static void dump_sector(const uint8_t *b)
{
    int i;
    int j;

    for (i = 0; i < EMMC_BLOCK_SIZE; i += 16) {
        k_printf("%03x:", (unsigned)i);
        for (j = 0; j < 16; j++) {
            k_printf(" %02x", (unsigned)b[i + j]);
        }
        k_printf("\n");
    }
}

static void read_sd(const struct board_info *board)
{
    struct emmc_card card;
    struct emmc_error err;
    uint32_t emmc_clk = 0;
    const uint8_t *b = (const uint8_t *)sector;
    const uint8_t *p0;
    uint32_t p0_lba;

    if (mbox_get_clock_rate(MBOX_CLOCK_EMMC, &emmc_clk) != 0) {
        emmc_clk = 0;
    }
    k_printf("@RC stage=spike2 ev=emmc base_clk=%u\n", (unsigned)emmc_clk);

    if (emmc_init(board->periph_base, emmc_clk, &card, &err) != 0) {
        print_error(&err);
        return;
    }
    k_printf("@RC stage=spike2 ev=card type=%s ocr=0x%08x rca=0x%04x cid=%08x%08x%08x%08x "
             "host_ver=%u\n",
             card.sdhc ? "sdhc" : "sdsc", (unsigned)card.ocr, (unsigned)card.rca,
             (unsigned)card.cid[0], (unsigned)card.cid[1], (unsigned)card.cid[2],
             (unsigned)card.cid[3], (unsigned)card.host_version);

    if (emmc_read_block(&card, 0, sector, &err) != 0) {
        print_error(&err);
        return;
    }

    p0 = b + 446;
    p0_lba = (uint32_t)p0[8] | ((uint32_t)p0[9] << 8) | ((uint32_t)p0[10] << 16) |
             ((uint32_t)p0[11] << 24);
    k_printf("@RC stage=spike2 ev=read lba=0 ok=1 sig=0x%04x crc32=0x%08x p0_type=0x%02x "
             "p0_lba=%u\n",
             (unsigned)(b[510] | (b[511] << 8)), (unsigned)crc32_iso_hdlc(b, EMMC_BLOCK_SIZE),
             (unsigned)p0[4], (unsigned)p0_lba);
    dump_sector(b);
}

void spike_main(uint32_t r0, uint32_t r1, uint32_t r2)
{
    struct board_info board;
    uint32_t board_rev = 0;
    uint32_t uart_clk = 0;
    int changed;
    int c;

    (void)r0;
    (void)r1;
    (void)r2;

    if (!board_detect(&board)) {
        for (;;) {
            __asm__ volatile("wfi");
        }
    }

    mbox_init(board.periph_base, board.bus_alias);
    if (mbox_get_clock_rate(MBOX_CLOCK_UART, &uart_clk) != 0) {
        uart_clk = UART_FALLBACK_CLOCK;
    }
    uart_init(board.periph_base, uart_clk, board.part != 0xd08);
    timer_init(board.periph_base);
    gpio_init(board.periph_base);

    if (mbox_get_board_rev(&board_rev) != 0) {
        board_rev = 0;
    }

    k_printf("\n@RC stage=spike2 ev=hello midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x\n",
             (unsigned)board.midr, board.soc, (unsigned)board.periph_base, (unsigned)board_rev);
    k_printf("RobCo spike 2: SD sector 0 through EMMC (read only)\n");

    print_fsel(34, 39);
    print_fsel(48, 53);

    if (!board.supported) {
        k_printf("warning: %s is unsupported, skipping SD\n", board.soc);
    } else {
        changed = remux_sd_to_emmc();
        if (changed) {
            print_fsel(34, 39);
            print_fsel(48, 53);
        }
        read_sd(&board);
    }

    k_printf("type to echo\n");
    for (;;) {
        c = uart_getc();
        uart_putc(c == '\r' ? '\n' : (char)c);
    }
}
