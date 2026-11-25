#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>
#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/mbox.h>
#include <robco/mmio.h>
#include <robco/sd.h>
#include <robco/timer.h>

/* Polled SD driver for the BCM283x EMMC controller (Arasan SDHCI). */

#define EMMC_OFFSET     0x300000

#define EMMC_BLKSIZECNT 0x04
#define EMMC_ARG1       0x08
#define EMMC_CMDTM      0x0c
#define EMMC_RESP0      0x10
#define EMMC_RESP1      0x14
#define EMMC_RESP2      0x18
#define EMMC_RESP3      0x1c
#define EMMC_DATA       0x20
#define EMMC_STATUS     0x24
#define EMMC_CONTROL0   0x28
#define EMMC_CONTROL1   0x2c
#define EMMC_INTERRUPT  0x30
#define EMMC_IRPT_MASK  0x34
#define EMMC_IRPT_EN    0x38
#define EMMC_CONTROL2   0x3c
#define EMMC_SLOTISR_VER 0xfc

#define SR_CMD_INHIBIT  (1u << 0)
#define SR_DAT_INHIBIT  (1u << 1)
#define SR_CARD_INSERTED (1u << 16)

#define C0_POWER_3V3    (0xfu << 8)

#define C1_CLK_INTLEN   (1u << 0)
#define C1_CLK_STABLE   (1u << 1)
#define C1_CLK_EN       (1u << 2)
#define C1_CLK_MASK     0xffe0u
#define C1_DATA_TOUNIT  (0xeu << 16)
#define C1_SRST_HC      (1u << 24)
#define C1_SRST_CMD     (1u << 25)
#define C1_SRST_DATA    (1u << 26)

#define INT_CMD_DONE    (1u << 0)
#define INT_DATA_DONE   (1u << 1)
#define INT_WRITE_RDY   (1u << 4)
#define INT_READ_RDY    (1u << 5)
#define INT_CTO_ERR     (1u << 16)
#define INT_DTO_ERR     (1u << 20)
#define INT_ERR_MASK    0xffff8000u

/* CMDTM: transfer mode in the low half, command in the high half. */
#define TM_BLKCNT_EN    (1u << 1)
#define TM_AUTO_CMD12   (1u << 2)
#define TM_DAT_DIR_READ (1u << 4)
#define TM_MULTI_BLOCK  (1u << 5)
#define CMD_RSP_NONE    (0u << 16)
#define CMD_RSP_136     (1u << 16)
#define CMD_RSP_48      (2u << 16)
#define CMD_RSP_48B     (3u << 16)
#define CMD_RSP_MASK    (3u << 16)
#define CMD_CRCCHK      (1u << 19)
#define CMD_IXCHK       (1u << 20)
#define CMD_ISDATA      (1u << 21)
#define CMD_TYPE_ABORT  (3u << 22)
#define CMD_INDEX(n)    ((uint32_t)(n) << 24)

#define R1              (CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK)
#define R1B             (CMD_RSP_48B | CMD_CRCCHK | CMD_IXCHK)
#define R2              (CMD_RSP_136 | CMD_CRCCHK)
#define R3              CMD_RSP_48

#define CMD_GO_IDLE     (CMD_INDEX(0) | CMD_RSP_NONE)
#define CMD_ALL_SEND_CID (CMD_INDEX(2) | R2)
#define CMD_SEND_RCA    (CMD_INDEX(3) | R1)
#define CMD_SELECT      (CMD_INDEX(7) | R1B)
#define CMD_SEND_IF_COND (CMD_INDEX(8) | R1)
#define CMD_SEND_CSD    (CMD_INDEX(9) | R2)
#define CMD_STOP        (CMD_INDEX(12) | R1B | CMD_TYPE_ABORT)
#define CMD_SEND_STATUS (CMD_INDEX(13) | R1)
#define CMD_SET_BLOCKLEN (CMD_INDEX(16) | R1)
#define CMD_READ_SINGLE (CMD_INDEX(17) | R1 | CMD_ISDATA | TM_DAT_DIR_READ)
#define CMD_READ_MULTI  (CMD_INDEX(18) | R1 | CMD_ISDATA | TM_DAT_DIR_READ | \
                         TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_WRITE_SINGLE (CMD_INDEX(24) | R1 | CMD_ISDATA)
#define CMD_WRITE_MULTI (CMD_INDEX(25) | R1 | CMD_ISDATA | \
                         TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_APP_CMD     (CMD_INDEX(55) | R1)
#define ACMD_SEND_OP    (CMD_INDEX(41) | R3)

#define OCR_BUSY_DONE   (1u << 31)
#define OCR_CCS         (1u << 30)
#define OCR_VDD_WINDOW  0x00ff8000u

/* R1 card status. */
#define R1_OUT_OF_RANGE (1u << 31)
#define R1_ADDRESS_ERR  (1u << 30)
#define R1_ERRORS       0xfdf90008u
#define R1_READY        (1u << 8)
#define R1_STATE(r)     (((r) >> 9) & 0xf)
#define STATE_TRAN      4

#define TIMEOUT_RESET_US   100000
#define TIMEOUT_CLOCK_US   100000
#define TIMEOUT_CMD_US     100000
#define TIMEOUT_DATA_US    1000000  /* per block, covers write busy too */
#define TIMEOUT_ACMD41_US  1000000
#define TIMEOUT_PROG_US    1000000

#define CLK_ID_HZ       400000
#define CLK_XFER_HZ     25000000
#define MBOX_CLOCK_EMMC 1

#define MAX_MULTI       0xffffu /* BLKSIZECNT count field */
#define XFER_TRIES      3

static uint32_t emmc_base;
static uint32_t host_version;
static int ready;
static int card_detect;         /* 1: the inserted bit was set at init, with a working card */
static sd_info_t info;
static blk_dev_t dev;

static inline uint32_t rd(uint32_t reg)
{
    return mmio_read(emmc_base + reg);
}

static inline void wr(uint32_t reg, uint32_t v)
{
    mmio_write(emmc_base + reg, v);
}

/* Waits until (reg & mask) == want. Returns 0 or BLK_ERR_TIMEOUT. */
static int wait_reg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
    uint64_t deadline = timer_now_us() + timeout_us;

    while ((rd(reg) & mask) != want) {
        if (timer_expired(deadline)) {
            return (rd(reg) & mask) == want ? 0 : BLK_ERR_TIMEOUT;
        }
    }
    return 0;
}

static int irq_error(uint32_t irq)
{
    return (irq & (INT_CTO_ERR | INT_DTO_ERR)) ? BLK_ERR_TIMEOUT : BLK_ERR_IO;
}

/* Waits for any bit of `mask` in INTERRUPT. Returns 0, BLK_ERR_IO or BLK_ERR_TIMEOUT. */
static int wait_int(uint32_t mask, uint32_t timeout_us)
{
    uint64_t deadline = timer_now_us() + timeout_us;
    uint32_t irq;

    for (;;) {
        irq = rd(EMMC_INTERRUPT);
        if (irq & INT_ERR_MASK) {
            return irq_error(irq);
        }
        if (irq & mask) {
            return 0;
        }
        if (timer_expired(deadline)) {
            irq = rd(EMMC_INTERRUPT);
            if ((irq & mask) && !(irq & INT_ERR_MASK)) {
                return 0;
            }
            return irq & INT_ERR_MASK ? irq_error(irq) : BLK_ERR_TIMEOUT;
        }
    }
}

static void reset_lines(void)
{
    wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
    (void)wait_reg(EMMC_CONTROL1, C1_SRST_CMD | C1_SRST_DATA, 0, TIMEOUT_RESET_US);
    wr(EMMC_INTERRUPT, 0xffffffffu);
}

/* SDHCI divided clock: f = base / (2 * N), N = 0 means base. */
static uint32_t clock_divider(uint32_t base, uint32_t target)
{
    uint32_t n;
    uint32_t p = 1;

    if (base <= target) {
        return 0;
    }
    n = (base + 2 * target - 1) / (2 * target);
    if (host_version >= 2) {
        return n > 0x3ff ? 0x3ff : n;   /* SDHCI 3.0: 10-bit divider */
    }
    while (p < n && p < 0x80) {         /* SDHCI 2.0: power of two */
        p <<= 1;
    }
    return p;
}

/* Returns the clock in Hz, or 0 on failure. */
static uint32_t set_clock(uint32_t base, uint32_t target)
{
    uint32_t div = clock_divider(base, target);
    uint32_t c1;

    if (wait_reg(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 0, TIMEOUT_CLOCK_US) != 0) {
        return 0;
    }
    c1 = rd(EMMC_CONTROL1) & ~C1_CLK_EN;
    wr(EMMC_CONTROL1, c1);
    timer_delay_us(10);

    c1 &= ~C1_CLK_MASK;
    c1 |= ((div & 0xff) << 8) | (((div >> 8) & 3) << 6) | C1_CLK_INTLEN;
    wr(EMMC_CONTROL1, c1);
    if (wait_reg(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, TIMEOUT_CLOCK_US) != 0) {
        return 0;
    }
    wr(EMMC_CONTROL1, c1 | C1_CLK_EN);
    timer_delay_us(2000);
    return div ? base / (2 * div) : base;
}

/* Issues one command and waits for CMD_DONE, and for busy end on R1b. */
static int send_cmd(uint32_t cmdtm, uint32_t arg)
{
    uint32_t inhibit = SR_CMD_INHIBIT;
    int rc;

    if ((cmdtm & CMD_ISDATA) || (cmdtm & CMD_RSP_MASK) == CMD_RSP_48B) {
        if ((cmdtm & CMD_TYPE_ABORT) != CMD_TYPE_ABORT) {
            inhibit |= SR_DAT_INHIBIT;
        }
    }
    if (wait_reg(EMMC_STATUS, inhibit, 0, TIMEOUT_CMD_US) != 0) {
        return BLK_ERR_TIMEOUT;
    }

    wr(EMMC_INTERRUPT, 0xffffffffu);
    wr(EMMC_ARG1, arg);
    wr(EMMC_CMDTM, cmdtm);

    rc = wait_int(INT_CMD_DONE, TIMEOUT_CMD_US);
    if (rc != 0) {
        return rc;
    }
    wr(EMMC_INTERRUPT, INT_CMD_DONE);

    if ((cmdtm & CMD_RSP_MASK) == CMD_RSP_48B && !(cmdtm & CMD_ISDATA)) {
        uint64_t deadline = timer_now_us() + TIMEOUT_DATA_US;

        for (;;) {
            uint32_t irq = rd(EMMC_INTERRUPT);

            if (irq & INT_ERR_MASK) {
                return irq_error(irq);
            }
            if (irq & INT_DATA_DONE) {
                wr(EMMC_INTERRUPT, INT_DATA_DONE);
                break;
            }
            if (!(rd(EMMC_STATUS) & SR_DAT_INHIBIT)) {
                break;
            }
            if (timer_expired(deadline)) {
                return BLK_ERR_TIMEOUT;
            }
        }
    }
    return 0;
}

static int send_acmd(uint32_t cmdtm, uint32_t arg)
{
    int rc = send_cmd(CMD_APP_CMD, info.rca << 16);

    return rc != 0 ? rc : send_cmd(cmdtm, arg);
}

/* Polls CMD13 until the card is back in the transfer state and ready. */
static int wait_card_ready(void)
{
    uint64_t deadline = timer_now_us() + TIMEOUT_PROG_US;
    uint32_t r;

    for (;;) {
        if (send_cmd(CMD_SEND_STATUS, info.rca << 16) == 0) {
            r = rd(EMMC_RESP0);
            if (R1_STATE(r) == STATE_TRAN && (r & R1_READY)) {
                return 0;
            }
        } else {
            reset_lines();
        }
        if (timer_expired(deadline)) {
            return BLK_ERR_TIMEOUT;
        }
        timer_delay_us(100);
    }
}

/* After a failed transfer: clean controller lines, stop any open transfer. */
static void recover(void)
{
    reset_lines();
    (void)send_cmd(CMD_STOP, 0);
    reset_lines();
    (void)wait_card_ready();
}

/*
 * Moves the SD card to the EMMC controller. On boards with WiFi the
 * firmware routes the card to SDHOST (GPIO 48-53 at ALT0) and EMMC to the
 * WiFi chip (GPIO 34-39 at ALT3). Never selects ALT0.
 */
static void remux_card_to_emmc(void)
{
    unsigned pin;
    int low = 0;
    int high = 0;

    for (pin = 34; pin <= 39; pin++) {
        low |= gpio_get_function(pin) == GPIO_FN_ALT3;
    }
    for (pin = 48; pin <= 53; pin++) {
        high |= gpio_get_function(pin) != GPIO_FN_ALT3;
    }
    for (pin = 34; low && pin <= 39; pin++) {
        gpio_set_function(pin, GPIO_FN_INPUT);
    }
    for (pin = 48; high && pin <= 53; pin++) {
        gpio_set_function(pin, GPIO_FN_ALT3);
    }
    if (low || high) {
        timer_delay_us(1000);
    }
}

/*
 * Reads `len` bits starting at bit `lo` of a 136-bit R2 register (CID or
 * CSD). The host strips the CRC byte, so RESP0 bit 0 is register bit 8.
 */
static uint32_t r2_bits(const uint32_t r[4], unsigned lo, unsigned len)
{
    uint32_t v = 0;
    unsigned k;

    for (k = len; k-- > 0;) {
        unsigned b = lo + k - 8;

        v = (v << 1) | ((r[b >> 5] >> (b & 31)) & 1u);
    }
    return v;
}

static void read_r2(uint32_t r[4])
{
    r[0] = rd(EMMC_RESP0);
    r[1] = rd(EMMC_RESP1);
    r[2] = rd(EMMC_RESP2);
    r[3] = rd(EMMC_RESP3);
}

static void parse_cid(const uint32_t r[4])
{
    unsigned i;

    info.cid[0] = r2_bits(r, 96, 32);
    info.cid[1] = r2_bits(r, 64, 32);
    info.cid[2] = r2_bits(r, 32, 32);
    info.cid[3] = r2_bits(r, 8, 24) << 8;
    info.mid = (uint8_t)r2_bits(r, 120, 8);
    info.oid[0] = (char)r2_bits(r, 112, 8);
    info.oid[1] = (char)r2_bits(r, 104, 8);
    info.oid[2] = 0;
    for (i = 0; i < 5; i++) {
        info.pnm[i] = (char)r2_bits(r, 96 - 8 * i, 8);
    }
    info.pnm[5] = 0;
    info.prv = (uint8_t)r2_bits(r, 56, 8);
    info.psn = r2_bits(r, 24, 32);
    info.mdt_year = (uint16_t)(2000 + r2_bits(r, 12, 8));
    info.mdt_month = (uint8_t)r2_bits(r, 8, 4);
}

/* Returns the capacity in 512-byte blocks, or 0 for an unknown layout. */
static uint64_t parse_csd(const uint32_t r[4])
{
    uint32_t structure = r2_bits(r, 126, 2);

    if (structure == 0) {
        uint64_t c_size = r2_bits(r, 62, 12);
        uint32_t mult = r2_bits(r, 47, 3);
        uint32_t bl_len = r2_bits(r, 80, 4);
        uint64_t bytes = (c_size + 1) << (mult + 2 + bl_len);

        return bytes / BLK_SECTOR_SIZE;
    }
    if (structure == 1) {
        return ((uint64_t)r2_bits(r, 48, 22) + 1) * 1024;
    }
    return 0;
}

static int identify(void)
{
    uint64_t deadline;
    uint32_t base_clk = 0;
    uint32_t arg;
    uint32_t r = 0;
    uint32_t reg[4];
    int v2 = 0;
    int cmd8_timeout = 0;
    int rc;

    /* Full host reset, then clock off, 1-bit bus, bus power on. */
    wr(EMMC_CONTROL2, 0);
    wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_HC);
    if (wait_reg(EMMC_CONTROL1, C1_SRST_HC, 0, TIMEOUT_RESET_US) != 0) {
        return SD_ERR_RESET;
    }
    wr(EMMC_CONTROL0, C0_POWER_3V3);
    wr(EMMC_CONTROL1, C1_DATA_TOUNIT);
    /* Latch every status bit, signal no interrupt to the ARM. */
    wr(EMMC_IRPT_EN, 0);
    wr(EMMC_IRPT_MASK, 0xffffffffu);
    wr(EMMC_INTERRUPT, 0xffffffffu);

    if (mbox_get_clock_rate(MBOX_CLOCK_EMMC, &base_clk) != 0 || base_clk == 0) {
        return SD_ERR_CLOCK;
    }
    info.clock_hz = set_clock(base_clk, CLK_ID_HZ);
    if (info.clock_hz == 0) {
        return SD_ERR_CLOCK;
    }

    if (send_cmd(CMD_GO_IDLE, 0) != 0) {
        return SD_ERR_CMD;
    }
    timer_delay_us(1000);

    /* CMD8: 2.7-3.6 V, check pattern 0xaa. A v1 card does not answer. */
    rc = send_cmd(CMD_SEND_IF_COND, 0x1aa);
    if (rc == 0) {
        if ((rd(EMMC_RESP0) & 0xfff) != 0x1aa) {
            return SD_ERR_VOLTAGE;
        }
        v2 = 1;
    } else {
        cmd8_timeout = rc == BLK_ERR_TIMEOUT;
        reset_lines();
    }

    arg = OCR_VDD_WINDOW | (v2 ? OCR_CCS : 0);
    deadline = timer_now_us() + TIMEOUT_ACMD41_US;
    for (;;) {
        rc = send_acmd(ACMD_SEND_OP, arg);
        if (rc != 0) {
            return (cmd8_timeout && rc == BLK_ERR_TIMEOUT) ? SD_ERR_NOCARD : SD_ERR_CMD;
        }
        r = rd(EMMC_RESP0);
        if (r & OCR_BUSY_DONE) {
            break;
        }
        if (timer_expired(deadline)) {
            return SD_ERR_BUSY;
        }
        timer_delay_us(10000);
    }
    if ((r & OCR_VDD_WINDOW) == 0) {
        return SD_ERR_VOLTAGE;
    }
    info.ocr = r;
    info.type = (r & OCR_CCS) ? SD_TYPE_SDHC : SD_TYPE_SDSC;

    if (send_cmd(CMD_ALL_SEND_CID, 0) != 0) {
        return SD_ERR_CMD;
    }
    read_r2(reg);
    parse_cid(reg);

    if (send_cmd(CMD_SEND_RCA, 0) != 0) {
        return SD_ERR_CMD;
    }
    info.rca = rd(EMMC_RESP0) >> 16;

    if (send_cmd(CMD_SEND_CSD, info.rca << 16) != 0) {
        return SD_ERR_CMD;
    }
    read_r2(reg);
    info.capacity_blocks = parse_csd(reg);
    if (info.capacity_blocks == 0) {
        return SD_ERR_CSD;
    }

    if (send_cmd(CMD_SELECT, info.rca << 16) != 0) {
        return SD_ERR_CMD;
    }
    info.clock_hz = set_clock(base_clk, CLK_XFER_HZ);
    if (info.clock_hz == 0) {
        return SD_ERR_CLOCK;
    }
    if (info.type == SD_TYPE_SDSC && send_cmd(CMD_SET_BLOCKLEN, BLK_SECTOR_SIZE) != 0) {
        return SD_ERR_CMD;
    }
    return 0;
}

/* Moves one block between the DATA register and `p`, which may be unaligned. */
static void pio_read_block(uint8_t *p)
{
    unsigned i;

    if (((uintptr_t)p & 3) == 0) {
        uint32_t *w = (uint32_t *)p;

        for (i = 0; i < BLK_SECTOR_SIZE / 4; i++) {
            w[i] = rd(EMMC_DATA);
        }
        return;
    }
    for (i = 0; i < BLK_SECTOR_SIZE; i += 4) {
        uint32_t v = rd(EMMC_DATA);

        p[i] = (uint8_t)v;
        p[i + 1] = (uint8_t)(v >> 8);
        p[i + 2] = (uint8_t)(v >> 16);
        p[i + 3] = (uint8_t)(v >> 24);
    }
}

static void pio_write_block(const uint8_t *p)
{
    unsigned i;

    if (((uintptr_t)p & 3) == 0) {
        const uint32_t *w = (const uint32_t *)p;

        for (i = 0; i < BLK_SECTOR_SIZE / 4; i++) {
            wr(EMMC_DATA, w[i]);
        }
        return;
    }
    for (i = 0; i < BLK_SECTOR_SIZE; i += 4) {
        wr(EMMC_DATA, (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
                      ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24));
    }
}

/* One command moving `n` blocks. Multi-block commands end with auto-CMD12. */
static int xfer(uint64_t lba, uint32_t n, uint8_t *buf, int write)
{
    uint32_t addr = info.type == SD_TYPE_SDHC ? (uint32_t)lba
                                              : (uint32_t)lba * BLK_SECTOR_SIZE;
    uint32_t cmd;
    uint32_t r1;
    uint32_t i;
    int rc;

    if (write) {
        cmd = n > 1 ? CMD_WRITE_MULTI : CMD_WRITE_SINGLE;
    } else {
        cmd = n > 1 ? CMD_READ_MULTI : CMD_READ_SINGLE;
    }
    wr(EMMC_BLKSIZECNT, (n << 16) | BLK_SECTOR_SIZE);
    rc = send_cmd(cmd, addr);
    if (rc != 0) {
        return rc;
    }
    r1 = rd(EMMC_RESP0);
    if (r1 & (R1_OUT_OF_RANGE | R1_ADDRESS_ERR)) {
        return BLK_ERR_RANGE;
    }
    if (r1 & R1_ERRORS) {
        return BLK_ERR_IO;
    }

    for (i = 0; i < n; i++) {
        uint32_t want = write ? INT_WRITE_RDY : INT_READ_RDY;

        rc = wait_int(want, TIMEOUT_DATA_US);
        if (rc != 0) {
            return rc;
        }
        wr(EMMC_INTERRUPT, want);
        if (write) {
            pio_write_block(buf + (size_t)i * BLK_SECTOR_SIZE);
        } else {
            pio_read_block(buf + (size_t)i * BLK_SECTOR_SIZE);
        }
    }

    rc = wait_int(INT_DATA_DONE, TIMEOUT_DATA_US);
    if (rc != 0) {
        return rc;
    }
    wr(EMMC_INTERRUPT, INT_DATA_DONE);
    return write ? wait_card_ready() : 0;
}

/* Tries a transfer a few times, cleaning up after each failure. */
static int xfer_retry(uint64_t lba, uint32_t n, uint8_t *buf, int write)
{
    int rc = BLK_ERR_IO;
    int t;

    for (t = 0; t < XFER_TRIES; t++) {
        rc = xfer(lba, n, buf, write);
        if (rc == 0 || rc == BLK_ERR_RANGE) {
            break;
        }
        recover();
    }
    return rc;
}

/*
 * After a failed transfer on a board that reports card detect: a clear
 * inserted bit means the card left. Report no medium until sd_init() runs
 * again. On a board that does not report it the bit is never read here,
 * and the error goes back to the caller as before.
 */
static int failed(int rc)
{
    if (rc != 0 && rc != BLK_ERR_RANGE && card_detect &&
        (rd(EMMC_STATUS) & SR_CARD_INSERTED) == 0) {
        ready = 0;
        return BLK_ERR_NOMEDIA;
    }
    return rc;
}

/*
 * Learns whether this board reports card detect. Called once the card has
 * answered and block 0 has been read: the card is in and working, so an
 * inserted bit that reads 0 now means the slot does not wire the detect
 * line to the controller. Only a bit that reads 1 here is trusted later.
 */
static void sample_card_detect(void)
{
    static uint8_t block[BLK_SECTOR_SIZE] __attribute__((aligned(4)));
    int rc = xfer_retry(0, 1, block, 0);

    info.present_state = rd(EMMC_STATUS);
    card_detect = rc == 0 && (info.present_state & SR_CARD_INSERTED) != 0;
    info.card_detect = (uint8_t)card_detect;
}

static int rw(uint64_t lba, uint32_t count, uint8_t *buf, int write)
{
    if (!ready) {
        return BLK_ERR_NOMEDIA;
    }
    if (buf == NULL || count == 0) {
        return BLK_ERR_PARAM;
    }
    if (lba >= info.capacity_blocks || count > info.capacity_blocks - lba) {
        return BLK_ERR_RANGE;
    }

    while (count > 0) {
        uint32_t n = count > MAX_MULTI ? MAX_MULTI : count;
        int rc = xfer_retry(lba, n, buf, write);
        uint32_t i;

        /* A failed multi-block transfer falls back to single blocks. */
        for (i = 0; rc != 0 && rc != BLK_ERR_RANGE && n > 1 && i < n; i++) {
            int one = xfer_retry(lba + i, 1, buf + (size_t)i * BLK_SECTOR_SIZE, write);

            if (one != 0) {
                return failed(one);
            }
            if (i == n - 1) {
                rc = 0;
            }
        }
        if (rc != 0) {
            return failed(rc);
        }
        lba += n;
        count -= n;
        buf += (size_t)n * BLK_SECTOR_SIZE;
    }
    return 0;
}

static int sd_read(blk_dev_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    return rw(lba, count, (uint8_t *)buf, 0);
}

static int sd_write(blk_dev_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    /* The buffer is only read on this path. */
    return rw(lba, count, (uint8_t *)(uintptr_t)buf, 1);
}

static int sd_sync(blk_dev_t *d)
{
    (void)d;
    return ready ? failed(wait_card_ready()) : BLK_ERR_NOMEDIA;
}

int sd_init(void)
{
    const robco_board_t *b = board_get();
    int rc;

    ready = 0;
    card_detect = 0;
    info = (sd_info_t){0};
    if (b->periph_base == 0 || b->soc == ROBCO_SOC_BCM2711 || b->soc == ROBCO_SOC_UNKNOWN) {
        return SD_ERR_BOARD;
    }
    emmc_base = b->periph_base + EMMC_OFFSET;
    host_version = (rd(EMMC_SLOTISR_VER) >> 16) & 0xff;

    remux_card_to_emmc();
    rc = identify();
    if (rc != 0) {
        info.type = SD_TYPE_NONE;
        return rc;
    }
    sample_card_detect();

    dev.name = "sd0";
    dev.block_size = BLK_SECTOR_SIZE;
    dev.block_count = info.capacity_blocks;
    dev.flags = BLK_FLAG_REMOVABLE;
    dev.read = sd_read;
    dev.write = sd_write;
    dev.sync = sd_sync;
    dev.priv = &info;
    ready = 1;
    return 0;
}

const char *sd_strerror(int err)
{
    switch (err) {
    case 0:              return "ok";
    case SD_ERR_BOARD:   return "board";
    case SD_ERR_CLOCK:   return "clock";
    case SD_ERR_RESET:   return "reset";
    case SD_ERR_NOCARD:  return "nocard";
    case SD_ERR_VOLTAGE: return "voltage";
    case SD_ERR_BUSY:    return "busy";
    case SD_ERR_CMD:     return "cmd";
    case SD_ERR_CSD:     return "csd";
    default:             return "unknown";
    }
}

blk_dev_t *sd_blkdev(void)
{
    return ready ? &dev : NULL;
}

const sd_info_t *sd_info(void)
{
    return ready ? &info : NULL;
}

void sd_deinit(void)
{
    if (emmc_base == 0) {
        return;
    }
    ready = 0;
    (void)wait_reg(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 0, TIMEOUT_CMD_US);
    wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) & ~C1_CLK_EN);
    wr(EMMC_CONTROL0, 0);
    wr(EMMC_INTERRUPT, 0xffffffffu);
}
