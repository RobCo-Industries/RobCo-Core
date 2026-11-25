/*
 * Holotape slot: an SD card on SPI0 behind a bus buffer (HOLO_BUS) and a
 * switched supply (HOLO_PWR). The steps follow the Pip-Boy HAT spec,
 * section 4.12, "Software sequence". The card protocol is src/lib/sdspi.c.
 *
 * Rules kept here:
 *   - Power goes on only after card detect stayed low HOLO_SETTLE_MS, and
 *     HOLO_OFF_MS after it last went off.
 *   - Power on, then bus on. Bus off, then power off.
 *   - The bus is on only around this driver's own transfers.
 *   - CS is a plain GPIO output, so the controller's own CE1 never reaches
 *     the card and the 80 start clocks run with CS high.
 *   - Before each transfer the driver sets the CS pin, MISO's pull and the
 *     SPI rate and mode again. It saves nothing and assumes nothing.
 *   - Any error, or card detect high, turns the bus and the power off.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>
#include <robco/gpio.h>
#include <robco/holo.h>
#include <robco/sd.h>
#include <robco/sdspi.h>
#include <robco/spi.h>
#include <robco/timer.h>

#define MISO_PIN    9u
/* SPI0 signals on the header: CE0 (the display), MISO, MOSI, SCLK. */
#define SPI0_FIRST_PIN 8u
#define SPI0_LAST_PIN  11u
/* The 40-pin header carries GPIO 0 to 27. */
#define HEADER_PIN_COUNT 28u
/* The display's chip select on SPI0. */
#define DISPLAY_SPI_CS 0u

static holo_config_t slot;
static int inited;
static sdspi_card_t card;
static blk_dev_t dev;
static int attached;
static int powered;
static int bus_on;
static int cd_low;          /* card detect read low at the last sample */
static uint64_t low_since;  /* when it went low */
static int was_present;
static uint64_t off_at;     /* when the supply last went off */

static int cd_reads_low(void)
{
    return gpio_read(slot.cd_pin) == 0;
}

/* Samples card detect and keeps the time it went low. Returns 1 while low. */
static int sample(void)
{
    if (!cd_reads_low()) {
        cd_low = 0;
        return 0;
    }
    if (!cd_low) {
        cd_low = 1;
        low_since = timer_now_us();
    }
    return 1;
}

static int settled(void)
{
    return sample() && timer_now_us() - low_since >= HOLO_SETTLE_MS * 1000ull;
}

static void set_bus(int on)
{
    (void)gpio_write(slot.bus_pin, on);
    bus_on = on;
}

static void set_power(int on)
{
    (void)gpio_write(slot.pwr_pin, on);
    if (powered && !on) {
        off_at = timer_now_us();
    }
    powered = on;
}

/* Bus low, then power low. The device stays and answers BLK_ERR_NOMEDIA. */
static void slot_off(void)
{
    if (bus_on) {
        (void)gpio_write(slot.cs_pin, 1);
        set_bus(0);
    }
    set_power(0);
    attached = 0;
    card.ready = 0;
    dev.block_count = 0;
    dev.flags = BLK_FLAG_REMOVABLE;
}

static int op_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    return spi_transfer(slot.spi_cs, tx, rx, len) < 0 ? -1 : 0;
}

static void op_cs(void *ctx, int high)
{
    (void)ctx;
    (void)gpio_write(slot.cs_pin, high);
}

static int op_set_hz(void *ctx, uint32_t hz)
{
    (void)ctx;
    return spi_configure(slot.spi_cs, hz, SPI_MODE_0) < 0 ? -1 : 0;
}

static void op_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    timer_delay_us(us);
}

static uint64_t op_now_us(void *ctx)
{
    (void)ctx;
    return timer_now_us();
}

static int op_present(void *ctx)
{
    (void)ctx;
    return cd_reads_low();
}

static const sdspi_ops_t slot_ops = {
    NULL, op_xfer, op_cs, op_set_hz, op_delay_us, op_now_us, op_present,
};

/* Everything a transfer needs, set again each time: CS pin, MISO pull, rate, mode. */
static int prepare(uint32_t hz)
{
    if (gpio_config_output(slot.cs_pin, 1) < 0 || gpio_set_pull(MISO_PIN, GPIO_PULL_NONE) < 0 ||
        spi_configure(slot.spi_cs, hz, SPI_MODE_0) < 0) {
        return -1;
    }
    return 0;
}

static int holo_read(blk_dev_t *d, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *p = buf;
    uint32_t i;
    int rc;

    (void)d;
    for (i = 0; i < count; i++) {
        if (!attached) {
            return BLK_ERR_NOMEDIA;
        }
        if (!cd_reads_low()) {
            slot_off();
            return BLK_ERR_NOMEDIA;
        }
        if (prepare(slot.hz) < 0) {
            slot_off();
            return BLK_ERR_IO;
        }
        set_bus(1);
        rc = sdspi_card_read(&card, lba + i, p + (size_t)i * BLK_SECTOR_SIZE);
        set_bus(0);
        if (!cd_reads_low()) {
            slot_off();
            return BLK_ERR_NOMEDIA;
        }
        if (rc != 0) {
            slot_off();
            return rc;
        }
    }
    return 0;
}

static int holo_write(blk_dev_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    return attached ? BLK_ERR_RO : BLK_ERR_NOMEDIA;
}

static int holo_sync(blk_dev_t *d)
{
    (void)d;
    return attached ? 0 : BLK_ERR_NOMEDIA;
}

int holo_init(const holo_config_t *c)
{
    static const holo_config_t def = HOLO_CONFIG_DEFAULT;
    uint8_t pins[5];
    unsigned i;
    unsigned j;

    if (c == NULL) {
        c = &def;
    }
    pins[0] = c->cd_pin;
    pins[1] = c->wp_pin;
    pins[2] = c->pwr_pin;
    pins[3] = c->bus_pin;
    pins[4] = c->cs_pin;
    for (i = 0; i < 5; i++) {
        if (pins[i] >= HEADER_PIN_COUNT ||
            (pins[i] >= SPI0_FIRST_PIN && pins[i] <= SPI0_LAST_PIN)) {
            return SD_ERR_BUS;
        }
        for (j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                return SD_ERR_BUS;
            }
        }
    }
    if (c->spi_cs >= SPI_CS_COUNT || c->spi_cs == DISPLAY_SPI_CS || c->hz < SDSPI_INIT_HZ) {
        return SD_ERR_BUS;
    }
    if (inited) {
        slot_off();
    }
    slot = *c;
    inited = 0;
    if (spi_get_hz(slot.spi_cs) == 0 && spi_init() < 0) {
        return SD_ERR_BUS;
    }
    /* Bus low first, then power: the order of the way down. */
    if (gpio_config_output(slot.bus_pin, 0) < 0 || gpio_config_output(slot.pwr_pin, 0) < 0 ||
        gpio_config_input(slot.cd_pin, GPIO_PULL_UP) < 0 ||
        gpio_config_input(slot.wp_pin, GPIO_PULL_UP) < 0 ||
        gpio_config_output(slot.cs_pin, 1) < 0 || gpio_set_pull(MISO_PIN, GPIO_PULL_NONE) < 0) {
        return SD_ERR_BUS;
    }
    bus_on = 0;
    powered = 0;
    attached = 0;
    card.ready = 0;
    cd_low = 0;
    was_present = 0;
    /* The supply may have been on before a reset: count the off time from now. */
    off_at = timer_now_us();
    dev = (blk_dev_t){0};
    dev.name = "holo0";
    dev.block_size = BLK_SECTOR_SIZE;
    dev.flags = BLK_FLAG_REMOVABLE;
    dev.read = holo_read;
    dev.write = holo_write;
    dev.sync = holo_sync;
    dev.priv = &card;
    inited = 1;
    return 0;
}

int holo_present(void)
{
    return inited && settled();
}

unsigned holo_status(void)
{
    unsigned s = 0;

    if (!inited) {
        return 0;
    }
    if (sample()) {
        s |= HOLO_ST_CARD;
    }
    if (settled()) {
        s |= HOLO_ST_PRESENT;
    }
    if (gpio_read(slot.wp_pin) == 1) {
        s |= HOLO_ST_LOCKED;
    }
    if (powered) {
        s |= HOLO_ST_POWER;
    }
    if (bus_on) {
        s |= HOLO_ST_BUS;
    }
    if (attached) {
        s |= HOLO_ST_ATTACHED;
    }
    return s;
}

int holo_attach(void)
{
    int rc;

    if (!inited) {
        return SD_ERR_BUS;
    }
    if (attached) {
        return 0;
    }
    if (!sample()) {
        return BLK_ERR_NOMEDIA;
    }
    if (!settled() || timer_now_us() - off_at < HOLO_OFF_MS * 1000ull) {
        return SD_ERR_AGAIN;
    }
    set_power(1);
    timer_delay_ms(HOLO_POWER_MS);
    if (!cd_reads_low()) {
        slot_off();
        return BLK_ERR_NOMEDIA;
    }
    if (prepare(SDSPI_INIT_HZ) < 0) {
        slot_off();
        return SD_ERR_BUS;
    }
    set_bus(1);
    rc = sdspi_card_init(&card, &slot_ops, slot.hz);
    set_bus(0);
    if (!cd_reads_low()) {
        slot_off();
        return BLK_ERR_NOMEDIA;
    }
    if (rc != 0) {
        slot_off();
        return rc;
    }
    dev.block_count = card.info.capacity_blocks;
    dev.flags = BLK_FLAG_READONLY | BLK_FLAG_REMOVABLE;
    attached = 1;
    return 0;
}

void holo_detach(void)
{
    if (inited) {
        slot_off();
    }
}

unsigned holo_poll(void)
{
    unsigned ev = 0;
    int now_present;

    if (!inited) {
        return 0;
    }
    if (!sample()) {
        if (attached || was_present) {
            ev |= HOLO_EV_REMOVED;
        }
        if (attached || powered) {
            slot_off();
        }
        was_present = 0;
        return ev;
    }
    now_present = settled();
    if (now_present && !was_present) {
        ev |= HOLO_EV_INSERTED;
    }
    was_present = now_present;
    return ev;
}

blk_dev_t *holo_blkdev(void)
{
    return attached ? &dev : NULL;
}

const sd_info_t *holo_info(void)
{
    return attached ? &card.info : NULL;
}
