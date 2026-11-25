/*
 * The card-detect rule of src/drv/sd_emmc.c against a fake EMMC controller
 * and a fake clock. The fake card answers every command, can fail a number
 * of data commands with a timeout, and can be pulled. One kind of board
 * wires the card-inserted bit (STATUS bit 16) to the slot, as QEMU does.
 * The other leaves it 0 with a card in, which a real Pi may do.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/mbox.h>
#include <robco/timer.h>

#define FAKE_BASE 0x3f000000u
#define EMMC      (FAKE_BASE + 0x300000u)

static uint32_t regs[0x100 / 4];
static uint64_t now;
static int card_in;             /* the card is in the slot */
static int board_reports;       /* the inserted bit follows the slot */
static unsigned fail_data;      /* data commands still to fail */
static unsigned data_cmds;
static uint32_t blocks_left, words_left;

#define R(off) regs[(off) / 4]

static uint32_t fake_read(uint32_t addr)
{
    uint32_t off = addr - EMMC;

    if (off == 0x24) {          /* STATUS: never inhibited */
        return board_reports && card_in ? 1u << 16 : 0;
    }
    if (off == 0x20) {          /* DATA */
        if (words_left > 0 && --words_left == 0) {
            R(0x30) |= --blocks_left > 0 ? 1u << 5 : 1u << 1;
            words_left = blocks_left > 0 ? 128 : 0;
        }
        return 0xa5a5a5a5u;
    }
    return off < sizeof(regs) ? R(off) : 0;
}

static void command(uint32_t cmdtm)
{
    uint32_t idx = cmdtm >> 24;
    int data = (cmdtm & (1u << 21)) != 0;

    if (!card_in) {
        /* CMD0 expects no response, so it completes with no card. */
        R(0x30) |= idx == 0 ? 1u << 0 : 1u << 16;
        return;
    }
    R(0x10) = 0;
    if (data) {
        data_cmds++;
        if (fail_data > 0) {
            fail_data--;
            R(0x30) |= 1u << 16;
            return;
        }
    }
    switch (idx) {
    case 8:  R(0x10) = 0x1aa; break;
    case 41: R(0x10) = 0x80000000u | 0x40000000u | 0x00ff8000u; break;
    case 3:  R(0x10) = 0x12340000u; break;
    case 9:  R(0x1c) = 1u << 22; break;            /* CSD version 2 */
    case 13: R(0x10) = (4u << 9) | (1u << 8); break; /* transfer state, ready */
    default: break;
    }
    R(0x30) |= 1u << 0;
    if (data) {
        blocks_left = (R(0x04) >> 16) != 0 ? R(0x04) >> 16 : 1;
        words_left = 128;
        R(0x30) |= (cmdtm & (1u << 4)) != 0 ? 1u << 5 : 1u << 4;
    } else if (idx == 7 || idx == 12) {
        R(0x30) |= 1u << 1;
    }
}

static void fake_write(uint32_t addr, uint32_t v)
{
    uint32_t off = addr - EMMC;

    switch (off) {
    case 0x30: R(0x30) &= ~v; break;                       /* write 1 to clear */
    case 0x2c: R(0x2c) = (v & ~(7u << 24)) | ((v & 1u) << 1); break; /* resets done, clock stable */
    case 0x0c: command(v); break;
    case 0x20:
        if (words_left > 0 && --words_left == 0) {
            R(0x30) |= --blocks_left > 0 ? 1u << 4 : 1u << 1;
            words_left = blocks_left > 0 ? 128 : 0;
        }
        break;
    default:
        if (off < sizeof(regs)) {
            R(off) = v;
        }
        break;
    }
}

#define ROBCO_MMIO_H
#define mmio_read(a) fake_read(a)
#define mmio_write(a, v) fake_write((a), (v))

#include "../../src/drv/sd_emmc.c"

#include "rc_test.h"

static robco_board_t board = { .soc = ROBCO_SOC_BCM2837, .periph_base = FAKE_BASE };

const robco_board_t *board_get(void)
{
    return &board;
}

uint64_t timer_now_us(void)
{
    return now += 100;
}

int timer_expired(uint64_t deadline_us)
{
    return (now += 100) >= deadline_us;
}

void timer_delay_us(uint32_t us)
{
    now += us;
}

int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz)
{
    (void)clock_id;
    *hz = 200000000u;
    return 0;
}

int gpio_get_function(unsigned pin)
{
    return pin >= 48 && pin <= 53 ? GPIO_FN_ALT3 : GPIO_FN_INPUT;
}

int gpio_set_function(unsigned pin, unsigned fn)
{
    (void)pin;
    (void)fn;
    return 0;
}

static blk_dev_t *start(int reports)
{
    memset(regs, 0, sizeof(regs));
    card_in = 1;
    board_reports = reports;
    fail_data = 0;
    words_left = blocks_left = 0;
    CHECK_EQ(sd_init(), 0);
    CHECK(sd_info() != NULL);
    CHECK_EQ(sd_info()->card_detect, reports);
    CHECK_EQ((sd_info()->present_state >> 16) & 1u, (uint32_t)reports);
    return sd_blkdev();
}

static uint8_t buf[4 * BLK_SECTOR_SIZE];

TEST(reporting_board_eject_is_no_medium)
{
    blk_dev_t *d = start(1);

    CHECK_EQ(blk_read(d, 8, 4, buf), 0);
    card_in = 0;
    CHECK_EQ(blk_read(d, 8, 1, buf), BLK_ERR_NOMEDIA);
    data_cmds = 0;
    CHECK_EQ(blk_read(d, 8, 1, buf), BLK_ERR_NOMEDIA);
    CHECK_EQ(data_cmds, 0);     /* no transfer until sd_init() */
    CHECK_EQ(blk_sync(d), BLK_ERR_NOMEDIA);
    card_in = 1;
    CHECK_EQ(sd_init(), 0);
    CHECK_EQ(blk_read(sd_blkdev(), 8, 1, buf), 0);
}

TEST(reporting_board_transient_error_retries)
{
    blk_dev_t *d = start(1);

    fail_data = 1;
    CHECK_EQ(blk_read(d, 8, 1, buf), 0);
    fail_data = XFER_TRIES;
    CHECK_EQ(blk_read(d, 8, 1, buf), BLK_ERR_TIMEOUT); /* the card is in: not gone */
    CHECK_EQ(blk_read(d, 8, 1, buf), 0);
}

TEST(silent_board_transient_error_is_not_gone)
{
    blk_dev_t *d = start(0);

    fail_data = 1;
    CHECK_EQ(blk_read(d, 8, 1, buf), 0);
    fail_data = 1;
    CHECK_EQ(blk_write(d, 8, 1, buf), 0);
    fail_data = XFER_TRIES;
    CHECK_EQ(blk_read(d, 8, 1, buf), BLK_ERR_TIMEOUT);
    CHECK_EQ(blk_read(d, 8, 1, buf), 0);
    CHECK_EQ(blk_sync(d), 0);
}

/* Without card detect a pull is a run of timeouts, never no medium. */
TEST(silent_board_pull_is_timeouts)
{
    blk_dev_t *d = start(0);
    int i;

    card_in = 0;
    for (i = 0; i < 3; i++) {
        CHECK_EQ(blk_read(d, 8, 1, buf), BLK_ERR_TIMEOUT);
    }
    CHECK_EQ(blk_sync(d), BLK_ERR_TIMEOUT);
    /* A new sd_init() is how the caller tells: no card answers. */
    CHECK_EQ(sd_init(), SD_ERR_NOCARD);
    CHECK(sd_blkdev() == NULL);
}

/* A card that fails its first block read leaves card detect off. */
TEST(failed_first_read_does_not_trust_bit)
{
    memset(regs, 0, sizeof(regs));
    card_in = 1;
    board_reports = 1;
    fail_data = XFER_TRIES;
    CHECK_EQ(sd_init(), 0);
    CHECK_EQ(sd_info()->card_detect, 0);
}
