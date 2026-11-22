#include "emmc.h"
#include "mmio.h"
#include "timer.h"

#define EMMC_OFFSET     0x300000

#define EMMC_ARG2       0x00
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

/* STATUS */
#define SR_CMD_INHIBIT  (1u << 0)
#define SR_DAT_INHIBIT  (1u << 1)

/* CONTROL0: SD bus power on at 3.3 V (SDHCI power control byte) */
#define C0_POWER_3V3    (0xfu << 8)

/* CONTROL1 */
#define C1_CLK_INTLEN   (1u << 0)
#define C1_CLK_STABLE   (1u << 1)
#define C1_CLK_EN       (1u << 2)
#define C1_CLK_MASK     0xffe0u
#define C1_DATA_TOUNIT  (0xeu << 16)
#define C1_SRST_HC      (1u << 24)
#define C1_SRST_CMD     (1u << 25)
#define C1_SRST_DATA    (1u << 26)

/* INTERRUPT */
#define INT_CMD_DONE    (1u << 0)
#define INT_DATA_DONE   (1u << 1)
#define INT_READ_RDY    (1u << 5)
#define INT_ERR         (1u << 15)
#define INT_CTO_ERR     (1u << 16)
#define INT_ERR_MASK    0xffff0000u

/* CMDTM */
#define TM_BLKCNT_EN    (1u << 1)
#define TM_DAT_DIR_READ (1u << 4)
#define CMD_RSP_NONE    (0u << 16)
#define CMD_RSP_136     (1u << 16)
#define CMD_RSP_48      (2u << 16)
#define CMD_RSP_48B     (3u << 16)
#define CMD_CRCCHK      (1u << 19)
#define CMD_IXCHK       (1u << 20)
#define CMD_ISDATA      (1u << 21)
#define CMD_INDEX(n)    ((uint32_t)(n) << 24)

#define R_NONE          CMD_RSP_NONE
#define R1              (CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK)
#define R1B             (CMD_RSP_48B | CMD_CRCCHK | CMD_IXCHK)
#define R2              (CMD_RSP_136 | CMD_CRCCHK)
#define R3              CMD_RSP_48
#define R6              R1
#define R7              R1

#define CMD_GO_IDLE         (CMD_INDEX(0) | R_NONE)
#define CMD_ALL_SEND_CID    (CMD_INDEX(2) | R2)
#define CMD_SEND_REL_ADDR   (CMD_INDEX(3) | R6)
#define CMD_SELECT_CARD     (CMD_INDEX(7) | R1B)
#define CMD_SEND_IF_COND    (CMD_INDEX(8) | R7)
#define CMD_SET_BLOCKLEN    (CMD_INDEX(16) | R1)
#define CMD_READ_SINGLE     (CMD_INDEX(17) | R1 | CMD_ISDATA | TM_DAT_DIR_READ)
#define CMD_APP_CMD         (CMD_INDEX(55) | R1)
#define ACMD_SD_SEND_OP     (CMD_INDEX(41) | R3)

#define OCR_BUSY_DONE   (1u << 31)
#define OCR_CCS         (1u << 30)
#define OCR_VDD_WINDOW  0x00ff8000u

#define TIMEOUT_RESET_US   100000
#define TIMEOUT_CLOCK_US   100000
#define TIMEOUT_CMD_US     100000
#define TIMEOUT_DATA_US    500000
#define TIMEOUT_ACMD41_US  1000000

#define CLK_ID_HZ       400000
#define CLK_XFER_HZ     25000000

static uintptr_t emmc_base;

static inline uint32_t rd(uint32_t reg)
{
    return mmio_read(emmc_base + reg);
}

static inline void wr(uint32_t reg, uint32_t v)
{
    mmio_write(emmc_base + reg, v);
}

static int fail(struct emmc_error *err, const char *step)
{
    err->step = step;
    err->status = rd(EMMC_STATUS);
    err->interrupt = rd(EMMC_INTERRUPT);
    return -1;
}

/* Waits until (reg & mask) == want. Returns 0 on success. */
static int wait_reg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
    uint32_t start = timer_now_us();

    for (;;) {
        if ((rd(reg) & mask) == want) {
            return 0;
        }
        if (timer_expired(start, timeout_us)) {
            return (rd(reg) & mask) == want ? 0 : -1;
        }
    }
}

static int reset_line(uint32_t bit)
{
    wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | bit);
    return wait_reg(EMMC_CONTROL1, bit, 0, TIMEOUT_RESET_US);
}

/* SDHCI divided clock: f = base / (2 * N), N = 0 means base. */
static uint32_t clock_divider(uint32_t base, uint32_t target, uint32_t host_version)
{
    uint32_t n;

    if (base <= target) {
        return 0;
    }
    n = (base + 2 * target - 1) / (2 * target);
    if (host_version >= 2) {
        /* SDHCI 3.0: 10-bit divider. */
        return n > 0x3ff ? 0x3ff : n;
    }
    /* SDHCI 2.0: 8-bit power of two. */
    {
        uint32_t p = 1;
        while (p < n && p < 0x80) {
            p <<= 1;
        }
        return p;
    }
}

static int set_clock(uint32_t base, uint32_t target, uint32_t host_version)
{
    uint32_t div = clock_divider(base, target, host_version);
    uint32_t c1;

    if (wait_reg(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 0, TIMEOUT_CLOCK_US) != 0) {
        return -1;
    }
    c1 = rd(EMMC_CONTROL1);
    c1 &= ~C1_CLK_EN;
    wr(EMMC_CONTROL1, c1);
    timer_delay_us(10);

    c1 &= ~C1_CLK_MASK;
    c1 |= ((div & 0xff) << 8) | (((div >> 8) & 3) << 6);
    c1 |= C1_CLK_INTLEN;
    wr(EMMC_CONTROL1, c1);
    if (wait_reg(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, TIMEOUT_CLOCK_US) != 0) {
        return -1;
    }
    wr(EMMC_CONTROL1, c1 | C1_CLK_EN);
    timer_delay_us(2000);
    return 0;
}

/* Issues one command and waits for CMD_DONE. Leaves data phase to the caller. */
static int send_cmd(uint32_t cmdtm, uint32_t arg, uint32_t timeout_us)
{
    uint32_t start;
    uint32_t irq;
    uint32_t inhibit = SR_CMD_INHIBIT;

    if (cmdtm & (CMD_ISDATA | CMD_RSP_48B)) {
        inhibit |= SR_DAT_INHIBIT;
    }
    if (wait_reg(EMMC_STATUS, inhibit, 0, TIMEOUT_CMD_US) != 0) {
        return -1;
    }

    wr(EMMC_INTERRUPT, 0xffffffffu);
    wr(EMMC_ARG1, arg);
    wr(EMMC_CMDTM, cmdtm);

    start = timer_now_us();
    for (;;) {
        irq = rd(EMMC_INTERRUPT);
        if (irq & (INT_ERR | INT_ERR_MASK)) {
            return -1;
        }
        if (irq & INT_CMD_DONE) {
            break;
        }
        if (timer_expired(start, timeout_us)) {
            return -1;
        }
    }
    wr(EMMC_INTERRUPT, INT_CMD_DONE);

    /* R1b: wait for the busy signal to end. */
    if ((cmdtm & (3u << 16)) == CMD_RSP_48B && !(cmdtm & CMD_ISDATA)) {
        start = timer_now_us();
        for (;;) {
            irq = rd(EMMC_INTERRUPT);
            if (irq & INT_DATA_DONE) {
                wr(EMMC_INTERRUPT, INT_DATA_DONE);
                break;
            }
            if (!(rd(EMMC_STATUS) & SR_DAT_INHIBIT)) {
                break;
            }
            if (irq & INT_ERR_MASK) {
                return -1;
            }
            if (timer_expired(start, timeout_us)) {
                return -1;
            }
        }
    }
    return 0;
}

static int send_acmd(uint32_t rca, uint32_t cmdtm, uint32_t arg)
{
    if (send_cmd(CMD_APP_CMD, rca << 16, TIMEOUT_CMD_US) != 0) {
        return -1;
    }
    return send_cmd(cmdtm, arg, TIMEOUT_CMD_US);
}

int emmc_init(uintptr_t periph_base, uint32_t base_clk, struct emmc_card *card,
              struct emmc_error *err)
{
    uint32_t start;
    uint32_t arg;
    uint32_t r;

    emmc_base = periph_base + EMMC_OFFSET;
    card->host_version = (rd(EMMC_SLOTISR_VER) >> 16) & 0xff;
    card->base_clk = base_clk;
    card->sdhc = 0;
    card->v2 = 0;
    card->ocr = 0;
    card->rca = 0;

    /* Full host reset, then clocks off and 1-bit bus. */
    wr(EMMC_CONTROL2, 0);
    wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_HC);
    if (wait_reg(EMMC_CONTROL1, C1_SRST_HC, 0, TIMEOUT_RESET_US) != 0) {
        return fail(err, "reset");
    }
    wr(EMMC_CONTROL0, C0_POWER_3V3);
    wr(EMMC_CONTROL1, C1_DATA_TOUNIT);
    /* Latch every status bit, signal no interrupt to the ARM. */
    wr(EMMC_IRPT_EN, 0);
    wr(EMMC_IRPT_MASK, 0xffffffffu);
    wr(EMMC_INTERRUPT, 0xffffffffu);

    if (base_clk == 0) {
        return fail(err, "base_clock");
    }
    if (set_clock(base_clk, CLK_ID_HZ, card->host_version) != 0) {
        return fail(err, "clock_400k");
    }

    if (send_cmd(CMD_GO_IDLE, 0, TIMEOUT_CMD_US) != 0) {
        return fail(err, "cmd0");
    }
    timer_delay_us(1000);

    /* CMD8: 2.7-3.6 V, check pattern 0xaa. A v1 card does not answer. */
    if (send_cmd(CMD_SEND_IF_COND, 0x1aa, TIMEOUT_CMD_US) == 0) {
        if ((rd(EMMC_RESP0) & 0xfff) != 0x1aa) {
            return fail(err, "cmd8_echo");
        }
        card->v2 = 1;
    } else {
        if (reset_line(C1_SRST_CMD) != 0) {
            return fail(err, "cmd8_reset");
        }
        wr(EMMC_INTERRUPT, 0xffffffffu);
    }

    arg = OCR_VDD_WINDOW | (card->v2 ? OCR_CCS : 0);
    start = timer_now_us();
    for (;;) {
        if (send_acmd(0, ACMD_SD_SEND_OP, arg) != 0) {
            return fail(err, "acmd41");
        }
        r = rd(EMMC_RESP0);
        if (r & OCR_BUSY_DONE) {
            break;
        }
        if (timer_expired(start, TIMEOUT_ACMD41_US)) {
            return fail(err, "acmd41_busy");
        }
        timer_delay_us(10000);
    }
    card->ocr = r;
    card->sdhc = (r & OCR_CCS) ? 1 : 0;

    if (send_cmd(CMD_ALL_SEND_CID, 0, TIMEOUT_CMD_US) != 0) {
        return fail(err, "cmd2");
    }
    card->cid[0] = rd(EMMC_RESP3);
    card->cid[1] = rd(EMMC_RESP2);
    card->cid[2] = rd(EMMC_RESP1);
    card->cid[3] = rd(EMMC_RESP0);

    if (send_cmd(CMD_SEND_REL_ADDR, 0, TIMEOUT_CMD_US) != 0) {
        return fail(err, "cmd3");
    }
    card->rca = rd(EMMC_RESP0) >> 16;

    if (send_cmd(CMD_SELECT_CARD, card->rca << 16, TIMEOUT_DATA_US) != 0) {
        return fail(err, "cmd7");
    }

    if (set_clock(base_clk, CLK_XFER_HZ, card->host_version) != 0) {
        return fail(err, "clock_25m");
    }

    if (!card->sdhc) {
        if (send_cmd(CMD_SET_BLOCKLEN, EMMC_BLOCK_SIZE, TIMEOUT_CMD_US) != 0) {
            return fail(err, "cmd16");
        }
    }
    return 0;
}

int emmc_read_block(const struct emmc_card *card, uint32_t lba, uint32_t *buf,
                    struct emmc_error *err)
{
    uint32_t addr = card->sdhc ? lba : lba * EMMC_BLOCK_SIZE;
    uint32_t start;
    uint32_t irq;
    int i;

    wr(EMMC_BLKSIZECNT, (1u << 16) | EMMC_BLOCK_SIZE);
    if (send_cmd(CMD_READ_SINGLE, addr, TIMEOUT_CMD_US) != 0) {
        return fail(err, "cmd17");
    }

    start = timer_now_us();
    for (;;) {
        irq = rd(EMMC_INTERRUPT);
        if (irq & INT_ERR_MASK) {
            return fail(err, "read_rdy");
        }
        if (irq & INT_READ_RDY) {
            break;
        }
        if (timer_expired(start, TIMEOUT_DATA_US)) {
            return fail(err, "read_rdy");
        }
    }
    wr(EMMC_INTERRUPT, INT_READ_RDY);

    for (i = 0; i < EMMC_BLOCK_SIZE / 4; i++) {
        buf[i] = rd(EMMC_DATA);
    }

    if (wait_reg(EMMC_INTERRUPT, INT_DATA_DONE, INT_DATA_DONE, TIMEOUT_DATA_US) != 0) {
        return fail(err, "data_done");
    }
    if (rd(EMMC_INTERRUPT) & INT_ERR_MASK) {
        return fail(err, "data_err");
    }
    wr(EMMC_INTERRUPT, INT_DATA_DONE);
    return 0;
}
