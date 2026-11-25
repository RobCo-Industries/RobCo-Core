/*
 * The holotape slot, src/drv/holo.c, on the real SPI0 driver over a fake
 * controller, fake GPIO and a fake clock, with the sd model of
 * sdcard_model.c behind a model of the slot's bus buffer:
 *
 *   - the sd hears a byte only while HOLO_PWR and HOLO_BUS are high,
 *   - its CS is GPIO 7 as a plain output, or CE1 while GPIO 7 is at ALT0,
 *   - MISO carries the sd's DO only while HOLO_BUS is high and CS low,
 *     and reads high otherwise, the board's pull-up.
 *
 * The ILI9341 driver runs on the same fake SPI0, on CE0, against the
 * panel model, for the sharing test. Any display byte sent while
 * HOLO_BUS is high counts as a violation: it would reach the sd.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <robco/blk.h>
#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/holo.h>
#include <robco/mbox.h>
#include <robco/sdspi.h>
#include <robco/tft.h>
#include <robco/timer.h>

#include "ili9341_model.h"
#include "sdcard_model.h"

#define FAKE_BASE   0x3f000000u
#define SPI_CS_REG  (FAKE_BASE + 0x204000u)
#define SPI_FIFO    (FAKE_BASE + 0x204004u)
#define SPI_CLK     (FAKE_BASE + 0x204008u)
#define CORE_HZ     250000000u

#define CD      12u
#define WP      13u
#define BUS     20u
#define PWR     21u
#define CS      7u
#define TFT_CS  8u
#define DC      25u
#define RST     4u

#define BLOCKS  4096u

/* The world. */
static uint64_t now_ns;
static uint32_t spi_cs;
static uint32_t spi_clk = 2;
static uint8_t rxq[64];
static unsigned rx_head;
static unsigned rx_n;

static unsigned fn[GPIO_PIN_COUNT];
static int level[GPIO_PIN_COUNT];
static unsigned pull[GPIO_PIN_COUNT];

static sdm_t sd;
static uint8_t *mem;
static int card_in;             /* sd in the socket: CD low */
static int flip_cd_on_vanish;   /* a vanished sd also opens CD */
static model_t panel;
static unsigned display_bytes;
static unsigned display_while_bus;
static unsigned power_on_count;

/* Pin log: every write to PWR or BUS, in order. */
static struct { uint8_t pin; uint8_t lvl; } plog[4096];
static unsigned plog_n;

static void tick(uint64_t ns) { now_ns += ns; }

static void pins_to_card(void)
{
    sdm_pins(&sd, card_in, level[PWR], level[BUS], now_ns / 1000u);
}

static void set_card(int in)
{
    card_in = in;
    level[CD] = !in;
    pins_to_card();
}

/* One byte on the wire: route it to the panel and the sd, return MISO. */
static uint8_t wire(uint8_t mosi)
{
    uint32_t hz = CORE_HZ / (spi_clk ? spi_clk : 65536u);
    int ta = (spi_cs >> 7) & 1u;
    int ce = (int)(spi_cs & 3u);
    int card_cs_low;
    uint8_t miso = 0xff;

    tick(8000000000ull / hz);
    if (fn[TFT_CS] == GPIO_FN_ALT0 && ta && ce == 0) {
        display_bytes++;
        model_byte(&panel, level[DC], mosi);
        if (level[BUS]) {
            display_while_bus++;
            sdm_violate(&sd, "display byte with HOLO_BUS high");
        }
    }
    if (fn[CS] == GPIO_FN_OUTPUT) {
        card_cs_low = !level[CS];
    } else {
        card_cs_low = fn[CS] == GPIO_FN_ALT0 && ta && ce == 1;
    }
    if (level[BUS] && level[PWR] && card_in) {
        uint8_t v = sdm_byte(&sd, card_cs_low, mosi, hz);

        if (card_cs_low) {
            miso = v;
        }
        if (flip_cd_on_vanish && (sd.gone || (sd.vanish_after && sd.bytes >= sd.vanish_after))) {
            set_card(0);
        }
    }
    return miso;
}

static uint32_t fake_read(uint32_t addr)
{
    tick(50);
    if (addr == SPI_CS_REG) {
        return (spi_cs & ~(3u << 4)) | (1u << 18) | (rx_n > 0 ? 1u << 17 : 0) | (1u << 16);
    }
    if (addr == SPI_FIFO && rx_n > 0) {
        uint8_t v = rxq[rx_head];

        rx_head = (rx_head + 1u) % sizeof(rxq);
        rx_n--;
        return v;
    }
    return 0;
}

static void fake_write(uint32_t addr, uint32_t v)
{
    tick(50);
    if (addr == SPI_CS_REG) {
        if (v & (3u << 4)) {
            rx_n = 0;
        }
        spi_cs = v & ~(3u << 4);
    } else if (addr == SPI_CLK) {
        spi_clk = v;
    } else if (addr == SPI_FIFO) {
        uint8_t m = wire((uint8_t)v);

        if (rx_n < sizeof(rxq)) {
            rxq[(rx_head + rx_n) % sizeof(rxq)] = m;
            rx_n++;
        }
    }
}

#define ROBCO_MMIO_H
#define mmio_read(a) fake_read(a)
#define mmio_write(a, v) fake_write((a), (v))

#include "../../src/drv/spi0.c"
#include "../../src/drv/holo.c"
#include "../../src/drv/tft_ili9341.c"

#include "rc_test.h"
#include "sdcard_model.c"

static robco_board_t board = { .periph_base = FAKE_BASE };

const robco_board_t *board_get(void) { return &board; }

int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz)
{
    (void)clock_id;
    *hz = CORE_HZ;
    return 0;
}

uint64_t timer_now_us(void) { tick(20); return now_ns / 1000u; }
uint32_t timer_now_lo(void) { return (uint32_t)timer_now_us(); }
uint32_t timer_now_ms(void) { return (uint32_t)(timer_now_us() / 1000u); }
int timer_expired(uint64_t deadline_us) { return timer_now_us() >= deadline_us; }
void timer_delay_us(uint32_t us) { tick((uint64_t)us * 1000u); }
void timer_delay_ms(uint32_t ms) { tick((uint64_t)ms * 1000000u); }

int gpio_set_function(unsigned pin, unsigned f)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    fn[pin] = f;
    return 0;
}

int gpio_get_function(unsigned pin)
{
    return pin < GPIO_PIN_COUNT ? (int)fn[pin] : GPIO_ERR_PIN;
}

int gpio_set_pull(unsigned pin, unsigned p)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    pull[pin] = p;
    return 0;
}

int gpio_write(unsigned pin, int lvl)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (pin == PWR || pin == BUS) {
        if (plog_n < sizeof(plog) / sizeof(plog[0])) {
            plog[plog_n].pin = (uint8_t)pin;
            plog[plog_n].lvl = (uint8_t)(lvl != 0);
            plog_n++;
        }
        if (pin == PWR && lvl && !level[PWR]) {
            power_on_count++;
        }
    }
    if (fn[pin] == GPIO_FN_OUTPUT || pin == PWR || pin == BUS || pin == CS) {
        level[pin] = lvl != 0;
    }
    if (pin == RST) {
        model_reset_pin(&panel, lvl != 0);
    }
    if (pin == PWR || pin == BUS) {
        pins_to_card();
    }
    return 0;
}

int gpio_read(unsigned pin)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    return level[pin];
}

int gpio_toggle(unsigned pin)
{
    return gpio_write(pin, !level[pin]);
}

int gpio_config_output(unsigned pin, int lvl)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    fn[pin] = GPIO_FN_OUTPUT;
    return gpio_write(pin, lvl);
}

int gpio_config_input(unsigned pin, unsigned p)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    fn[pin] = GPIO_FN_INPUT;
    pull[pin] = p;
    return 0;
}

/* Test helpers. */

#define NO_VIOLATION() CHECK_STR(sd.violations ? sd.first : "none", "none")
#define SLOT_OFF() do { CHECK_EQ(level[BUS], 0); CHECK_EQ(level[PWR], 0); } while (0)

static void fill(void)
{
    uint32_t i;

    if (mem == NULL) {
        mem = malloc((size_t)BLOCKS * 512u);
    }
    for (i = 0; i < BLOCKS * 512u; i++) {
        mem[i] = (uint8_t)((i * 40503u) >> 7);
    }
}

/* A fresh world: pins at their reset state, a sd of `kind` in or out. */
static void world(int kind, int in)
{
    fill();
    memset(fn, 0, sizeof(fn));
    memset(level, 0, sizeof(level));
    memset(pull, 0, sizeof(pull));
    level[CS] = 1;
    level[WP] = 1;
    sdm_init(&sd, kind, mem, BLOCKS);
    card_in = 0;
    sdm_pins(&sd, 0, 0, 0, 0);
    sd.violations = 0;
    sd.first[0] = 0;
    now_ns = 5000000000ull;
    flip_cd_on_vanish = 0;
    display_bytes = display_while_bus = power_on_count = 0;
    plog_n = 0;
    model_init(&panel);
    set_card(in);
    level[WP] = in ? 0 : 1;
    CHECK_EQ(spi_init(), 0);
    CHECK_EQ(holo_init(NULL), 0);
}

static void wait_ms(unsigned ms)
{
    timer_delay_ms(ms);
}

/* The main loop sees the card, then the debounce runs out. */
static void settle(void)
{
    (void)holo_poll();
    wait_ms(HOLO_SETTLE_MS + 1u);
    (void)holo_poll();
}

/* Init, wait out the debounce, attach. */
static int bring_up(int kind)
{
    world(kind, 1);
    settle();
    return holo_attach();
}

TEST(init_sets_safe_pins_and_powers_nothing)
{
    world(SDM_SDHC, 1);
    CHECK_EQ(fn[PWR], GPIO_FN_OUTPUT);
    CHECK_EQ(fn[BUS], GPIO_FN_OUTPUT);
    SLOT_OFF();
    CHECK_EQ(fn[CS], GPIO_FN_OUTPUT);
    CHECK_EQ(level[CS], 1);
    CHECK_EQ(fn[CD], GPIO_FN_INPUT);
    CHECK_EQ(fn[WP], GPIO_FN_INPUT);
    CHECK_EQ(pull[CD], GPIO_PULL_UP);
    CHECK_EQ(pull[9], GPIO_PULL_NONE);
    CHECK_EQ(power_on_count, 0);
    CHECK(holo_blkdev() == NULL);
    CHECK(holo_info() == NULL);
    NO_VIOLATION();
}

TEST(init_rejects_bad_config)
{
    holo_config_t c = HOLO_CONFIG_DEFAULT;

    world(SDM_SDHC, 0);
    c.bus_pin = c.pwr_pin;
    CHECK_EQ(holo_init(&c), SD_ERR_BUS);
    c = (holo_config_t)HOLO_CONFIG_DEFAULT;
    c.cs_pin = 9;
    CHECK_EQ(holo_init(&c), SD_ERR_BUS);
    c = (holo_config_t)HOLO_CONFIG_DEFAULT;
    c.spi_cs = 2;
    CHECK_EQ(holo_init(&c), SD_ERR_BUS);
    c = (holo_config_t)HOLO_CONFIG_DEFAULT;
    c.hz = 1000;
    CHECK_EQ(holo_init(&c), SD_ERR_BUS);
}

TEST(debounce_by_timestamps)
{
    unsigned ev;

    world(SDM_SDHC, 0);
    CHECK_EQ(holo_present(), 0);
    CHECK_EQ(holo_poll(), 0);
    set_card(1);
    CHECK_EQ(holo_poll(), 0);
    CHECK_EQ(holo_status() & (HOLO_ST_CARD | HOLO_ST_PRESENT), HOLO_ST_CARD);
    wait_ms(HOLO_SETTLE_MS - 10u);
    CHECK_EQ(holo_present(), 0);
    CHECK_EQ(holo_attach(), SD_ERR_AGAIN);
    /* A bounce restarts the count. */
    set_card(0);
    CHECK_EQ(holo_poll(), 0);
    set_card(1);
    (void)holo_poll();
    wait_ms(20);
    CHECK_EQ(holo_present(), 0);
    wait_ms(HOLO_SETTLE_MS);
    CHECK_EQ(holo_present(), 1);
    ev = holo_poll();
    CHECK_EQ(ev, HOLO_EV_INSERTED);
    CHECK_EQ(holo_poll(), 0);
    set_card(0);
    CHECK_EQ(holo_poll(), HOLO_EV_REMOVED);
    CHECK_EQ(power_on_count, 0);
    NO_VIOLATION();
}

TEST(attach_with_no_card_never_powers)
{
    world(SDM_SDHC, 0);
    wait_ms(1000);
    CHECK_EQ(holo_attach(), BLK_ERR_NOMEDIA);
    CHECK_EQ(power_on_count, 0);
    SLOT_OFF();
}

TEST(attach_sdhc_follows_the_sequence)
{
    unsigned i;
    int pwr_up = -1;
    int bus_up = -1;
    int bus_down = -1;
    blk_dev_t *d;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    NO_VIOLATION();
    for (i = 0; i < plog_n; i++) {
        if (plog[i].pin == PWR && plog[i].lvl && pwr_up < 0) {
            pwr_up = (int)i;
        }
        if (plog[i].pin == BUS && plog[i].lvl && bus_up < 0) {
            bus_up = (int)i;
        }
        if (plog[i].pin == BUS && !plog[i].lvl && bus_up >= 0) {
            bus_down = (int)i;
        }
    }
    CHECK(pwr_up >= 0 && bus_up > pwr_up && bus_down > bus_up);
    CHECK_EQ(level[PWR], 1);
    CHECK_EQ(level[BUS], 0);
    CHECK_EQ(level[CS], 1);
    CHECK_EQ(sd.crc_on, 1);
    d = holo_blkdev();
    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK_STR(d->name, "holo0");
    CHECK_EQ(d->block_count, BLOCKS);
    CHECK_EQ(d->flags, BLK_FLAG_READONLY | BLK_FLAG_REMOVABLE);
    CHECK_EQ(holo_info()->type, SD_TYPE_SDHC);
    CHECK_EQ(holo_info()->clock_hz, HOLO_HZ_DEFAULT);
    CHECK_EQ(spi_get_hz(1), 9615384u);
    CHECK_EQ(holo_status() & (HOLO_ST_ATTACHED | HOLO_ST_POWER | HOLO_ST_BUS | HOLO_ST_LOCKED),
             HOLO_ST_ATTACHED | HOLO_ST_POWER);
    CHECK_EQ(blk_write(d, 0, 1, mem), BLK_ERR_RO);
    CHECK_EQ(blk_sync(d), 0);
}

TEST(read_only_whatever_the_lock_tab)
{
    CHECK_EQ(bring_up(SDM_SDSC), 0);
    level[WP] = 1;
    CHECK(holo_status() & HOLO_ST_LOCKED);
    CHECK_EQ(holo_blkdev()->flags & BLK_FLAG_READONLY, BLK_FLAG_READONLY);
    level[WP] = 0;
    CHECK_EQ(holo_status() & HOLO_ST_LOCKED, 0);
    CHECK_EQ(holo_blkdev()->flags & BLK_FLAG_READONLY, BLK_FLAG_READONLY);
    CHECK_EQ(blk_write(holo_blkdev(), 0, 1, mem), BLK_ERR_RO);
}

static void reads(int kind)
{
    uint8_t *b = malloc(512);
    uint8_t raw[515];
    blk_dev_t *d;

    CHECK_EQ(bring_up(kind), 0);
    d = holo_blkdev();
    CHECK_EQ(blk_read(d, 0, 1, b), 0);
    CHECK_MEM(b, mem, 512);
    CHECK_EQ(level[BUS], 0);
    CHECK_EQ(blk_read(d, BLOCKS - 1u, 1, b), 0);
    CHECK_MEM(b, mem + (size_t)(BLOCKS - 1u) * 512u, 512);
    CHECK_EQ(blk_read(d, BLOCKS, 1, b), BLK_ERR_RANGE);
    CHECK_EQ(blk_read(d, BLOCKS - 1u, 2, b), BLK_ERR_RANGE);
    memset(raw, 0x5a, sizeof(raw));
    CHECK_EQ(blk_read(d, 11, 1, raw + 1), 0);
    CHECK_MEM(raw + 1, mem + 11u * 512u, 512);
    CHECK_EQ(raw[0], 0x5a);
    CHECK_EQ(raw[513], 0x5a);
    CHECK_EQ(level[BUS], 0);
    CHECK_EQ(level[PWR], 1);
    CHECK_EQ(level[CS], 1);
    NO_VIOLATION();
    free(b);
}

TEST(reads_sdhc) { reads(SDM_SDHC); }
TEST(reads_sdsc) { reads(SDM_SDSC); }

TEST(read_4096_blocks_in_one_call)
{
    uint8_t *b = malloc((size_t)BLOCKS * 512u);
    uint64_t t0;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    t0 = now_ns;
    CHECK_EQ(blk_read(holo_blkdev(), 0, BLOCKS, b), 0);
    CHECK_EQ(memcmp(b, mem, (size_t)BLOCKS * 512u), 0);
    CHECK_EQ(sd.reads, BLOCKS);
    CHECK_EQ(level[BUS], 0);
    /* Fake wire time for 2 MiB at 9.62 MHz, plus overhead. */
    printf("  2 MiB in %llu ms of fake time\n", (unsigned long long)((now_ns - t0) / 1000000u));
    NO_VIOLATION();
    free(b);
}

TEST(detach_then_every_call_is_nomedia)
{
    uint8_t b[512];
    blk_dev_t *d;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    d = holo_blkdev();
    holo_detach();
    SLOT_OFF();
    CHECK(holo_blkdev() == NULL);
    CHECK(holo_info() == NULL);
    CHECK_EQ(blk_read(d, 0, 1, b), BLK_ERR_NOMEDIA);
    CHECK_EQ(blk_read(d, BLOCKS + 5u, 1, b), BLK_ERR_NOMEDIA);
    CHECK_EQ(blk_write(d, 0, 1, b), BLK_ERR_NOMEDIA);
    CHECK_EQ(blk_sync(d), BLK_ERR_NOMEDIA);
    NO_VIOLATION();
}

TEST(off_time_enforced)
{
    CHECK_EQ(bring_up(SDM_SDHC), 0);
    holo_detach();
    CHECK_EQ(holo_attach(), SD_ERR_AGAIN);
    wait_ms(HOLO_OFF_MS - 5u);
    CHECK_EQ(holo_attach(), SD_ERR_AGAIN);
    CHECK_EQ(power_on_count, 1);
    wait_ms(5);
    CHECK_EQ(holo_attach(), 0);
    CHECK_EQ(power_on_count, 2);
    NO_VIOLATION();
}

TEST(poll_detaches_at_once_on_removal)
{
    uint8_t b[512];
    blk_dev_t *d;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    d = holo_blkdev();
    CHECK_EQ(holo_poll(), 0);
    set_card(0);
    CHECK_EQ(holo_poll(), HOLO_EV_REMOVED);
    SLOT_OFF();
    CHECK_EQ(blk_read(d, 0, 1, b), BLK_ERR_NOMEDIA);
    CHECK_EQ(holo_attach(), BLK_ERR_NOMEDIA);
    NO_VIOLATION();
}

TEST(reattach_after_removal)
{
    uint8_t b[512];

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    set_card(0);
    (void)holo_poll();
    wait_ms(30);
    set_card(1);
    CHECK_EQ(holo_attach(), SD_ERR_AGAIN);
    wait_ms(HOLO_SETTLE_MS);
    CHECK_EQ(holo_poll(), HOLO_EV_INSERTED);
    CHECK_EQ(holo_attach(), 0);
    CHECK_EQ(blk_read(holo_blkdev(), 5, 1, b), 0);
    CHECK_MEM(b, mem + 5u * 512u, 512);
    NO_VIOLATION();
}

TEST(no_answer_fails_by_timeout_slot_off)
{
    uint64_t t0;

    world(SDM_SDHC, 1);
    sd.faults = SDM_F_NOANSWER;
    sd.gone = 1;
    settle();
    t0 = now_ns;
    CHECK_EQ(holo_attach(), SD_ERR_NOCARD);
    CHECK((now_ns - t0) / 1000u >= SDSPI_CMD0_US);
    CHECK((now_ns - t0) / 1000u < SDSPI_CMD0_US + 50000u);
    SLOT_OFF();
    CHECK(holo_blkdev() == NULL);
}

static void init_fault(unsigned fault, int want)
{
    world(SDM_SDHC, 1);
    sd.faults = fault;
    settle();
    CHECK_EQ(holo_attach(), want);
    SLOT_OFF();
    CHECK_EQ(level[CS], 1);
}

TEST(init_faults_leave_slot_off)
{
    init_fault(SDM_F_BAD_ECHO, SD_ERR_VOLTAGE);
    init_fault(SDM_F_NEVER_READY, SD_ERR_BUSY);
    init_fault(SDM_F_BAD_CSD, SD_ERR_CSD);
    world(SDM_V1, 1);
    settle();
    CHECK_EQ(holo_attach(), SD_ERR_OLD);
    SLOT_OFF();
    world(SDM_MMC, 1);
    settle();
    CHECK_EQ(holo_attach(), SD_ERR_OLD);
    SLOT_OFF();
}

static void read_fault(unsigned fault, int want)
{
    uint8_t b[512];
    blk_dev_t *d;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    d = holo_blkdev();
    sd.faults = fault;
    CHECK_EQ(blk_read(d, 1, 1, b), want);
    SLOT_OFF();
    CHECK_EQ(blk_read(d, 1, 1, b), BLK_ERR_NOMEDIA);
    NO_VIOLATION();
}

TEST(read_faults_leave_slot_off)
{
    read_fault(SDM_F_BAD_DCRC, BLK_ERR_IO);
    read_fault(SDM_F_ERR_TOKEN, BLK_ERR_IO);
    read_fault(SDM_F_NO_TOKEN, BLK_ERR_TIMEOUT);
}

/* Pulled after n bytes of an attach: CD opens and MISO reads high from then on. */
TEST(removal_at_every_byte_of_attach)
{
    uint64_t total;
    uint64_t n;
    unsigned bad = 0;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    total = sd.bytes;
    CHECK(total > 100);
    for (n = 1; n <= total; n++) {
        uint64_t t0;
        int rc;

        world(SDM_SDHC, 1);
        settle();
        sd.vanish_after = n;
        flip_cd_on_vanish = 1;
        t0 = now_ns;
        rc = holo_attach();
        if (rc != BLK_ERR_NOMEDIA || level[BUS] || level[PWR] || holo_blkdev() != NULL ||
            (now_ns - t0) / 1000u > 50000u || sd.violations) {
            bad++;
            printf("  pulled after %llu: rc=%d bus=%d pwr=%d %s\n", (unsigned long long)n, rc,
                   level[BUS], level[PWR], sd.first);
        }
    }
    CHECK_EQ(bad, 0);
}

TEST(removal_at_every_byte_of_read)
{
    uint8_t *b = malloc(512);
    uint64_t base;
    uint64_t total;
    uint64_t n;
    unsigned bad = 0;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    base = sd.bytes;
    CHECK_EQ(blk_read(holo_blkdev(), 3, 1, b), 0);
    total = sd.bytes - base;
    for (n = 0; n <= total; n++) {
        int rc;

        CHECK_EQ(bring_up(SDM_SDHC), 0);
        sd.vanish_after = sd.bytes + n;
        if (n == 0) {
            sd.gone = 1;
        }
        flip_cd_on_vanish = 1;
        if (n == 0) {
            set_card(0);
        }
        rc = blk_read(holo_blkdev(), 3, 1, b);
        if (rc != BLK_ERR_NOMEDIA || level[BUS] || level[PWR] || holo_blkdev() != NULL) {
            bad++;
            printf("  pulled after %llu of %llu: rc=%d bus=%d pwr=%d\n", (unsigned long long)n,
                   (unsigned long long)total, rc, level[BUS], level[PWR]);
        }
    }
    CHECK_EQ(bad, 0);
    free(b);
}

/* Pulled in the middle of a multi-block read: no block after it is read. */
TEST(removal_mid_multi_block_read)
{
    uint8_t *b = malloc(64u * 512u);

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    flip_cd_on_vanish = 1;
    sd.vanish_after = sd.bytes + 10u * 540u;
    CHECK_EQ(blk_read(holo_blkdev(), 0, 64, b), BLK_ERR_NOMEDIA);
    CHECK(sd.reads < 12);
    SLOT_OFF();
    free(b);
}

/* Display sends and sd reads take turns on SPI0. Both stay right. */
TEST(display_and_card_share_spi0)
{
    static const tft_config_t tc = TFT_CONFIG_DEFAULT;
    static uint16_t px[32 * 32];
    uint8_t *b = malloc(512);
    unsigned i;
    unsigned j;
    int bad = 0;
    static uint16_t view[MODEL_W * MODEL_H];
    unsigned pixels0;
    unsigned w;

    world(SDM_SDHC, 1);
    CHECK_EQ(tft_init(&tc), 0);
    settle();
    CHECK_EQ(holo_attach(), 0);
    pixels0 = panel.pixels;
    for (i = 0; i < 64; i++) {
        uint16_t colour = (uint16_t)(0x1234u * (i + 1u));

        for (j = 0; j < 32u * 32u; j++) {
            px[j] = (uint16_t)(colour + j);
        }
        bad |= tft_write_rect((i % 8u) * 32u, (i / 8u) % 7u * 32u, 32, 32, px) != 0;
        bad |= blk_read(holo_blkdev(), i * 37u, 1, b) != 0;
        bad |= memcmp(b, mem + (size_t)i * 37u * 512u, 512) != 0;
        bad |= level[BUS] != 0;
        /* Someone starts SPI0 again: GPIO 7 goes back to ALT0. The next read must cope. */
        if (i == 20) {
            CHECK_EQ(spi_init(), 0);
            CHECK_EQ(fn[CS], GPIO_FN_ALT0);
        }
        if (i % 9u == 0) {
            bad |= tft_fill_rect(0, 0, 8, 8, colour) != 0;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(display_bytes > 64u * 32u * 32u * 2u);
    CHECK_EQ(display_while_bus, 0);
    CHECK_EQ(tft_get_hz(), 9615384u);
    CHECK_EQ(spi_get_hz(1), 9615384u);
    /* Every pixel arrived, and the last rectangle shows where it should. */
    CHECK_EQ(panel.pixels - pixels0, 64u * 32u * 32u + 8u * 64u);
    w = model_view(&panel, tc.rotation, view);
    for (j = 0; j < 32u * 32u; j++) {
        bad |= view[(j / 32u) * w + 224u + j % 32u] != (uint16_t)(0x1234u * 64u + j);
    }
    CHECK_EQ(bad, 0);
    NO_VIOLATION();
    free(b);
}

/* The checker itself: a display byte with HOLO_BUS high is a violation. */
TEST(display_with_bus_high_is_caught)
{
    static const tft_config_t tc = TFT_CONFIG_DEFAULT;

    CHECK_EQ(bring_up(SDM_SDHC), 0);
    CHECK_EQ(tft_init(&tc), 0);
    (void)gpio_write(BUS, 1);
    CHECK_EQ(tft_fill_rect(0, 0, 4, 4, 0xffff), 0);
    CHECK(display_while_bus > 0);
    CHECK(sd.violations > 0);
}
