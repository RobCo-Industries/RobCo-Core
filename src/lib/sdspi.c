#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>
#include <robco/sd.h>
#include <robco/sdspi.h>

/*
 * SD card protocol in SPI mode. Names and values follow the SD Physical
 * Layer Simplified Specification, sections 7.2 and 7.3. Every wait has a
 * deadline on now_us() and a cap on its loop count, so a stuck clock
 * cannot hang it either.
 */

#define R1_IDLE         0x01u
#define R1_ILLEGAL      0x04u
#define R1_CRC          0x08u
#define R1_ADDRESS      0x20u
#define R1_PARAM        0x40u

#define TOKEN_START     0xfeu
#define TOKEN_RANGE     0x08u   /* data error token: out of range */

#define OCR_BUSY        0x80000000u     /* set when power up is done */
#define OCR_CCS         0x40000000u     /* card capacity status: block addressing */
#define OCR_33V         0x00300000u     /* 3.2 V to 3.4 V */
#define ACMD41_HCS      0x40000000u

#define RETRY_US        1000u   /* pause between CMD0 or ACMD41 tries */

/* Internal results, below every R1 value. */
#define E_BUS           (-1)    /* xfer() or set_hz() failed */
#define E_NORESP        (-2)    /* no R1 within Ncr */
#define E_GONE          (-3)    /* present() said the card is gone */
#define E_TIMEOUT       (-4)    /* no data token in time */
#define E_TOKEN         (-5)    /* data error token, not out of range */
#define E_TOKEN_RANGE   (-6)    /* data error token: out of range */
#define E_CRC           (-7)    /* data CRC16 mismatch */
#define E_GARBAGE       (-8)    /* a byte that is neither 0xFF nor a token */

/* MOSI stays high whenever the host only reads. */
static const uint8_t ones[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

uint8_t sdspi_crc7(const uint8_t *p, size_t len)
{
    uint8_t crc = 0;
    size_t i;
    unsigned b;

    for (i = 0; i < len; i++) {
        uint8_t d = p[i];

        for (b = 0; b < 8; b++) {
            crc = (uint8_t)(crc << 1);
            if ((d ^ crc) & 0x80u) {
                crc ^= 0x09u;
            }
            d = (uint8_t)(d << 1);
        }
    }
    return (uint8_t)((crc << 1) | 1u);
}

uint16_t sdspi_crc16(const uint8_t *p, size_t len)
{
    uint16_t crc = 0;
    size_t i;
    unsigned b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int gone(const sdspi_card_t *c)
{
    return c->ops->present != NULL && !c->ops->present(c->ops->ctx);
}

/* Reads len bytes with MOSI high. */
static int read_bytes(const sdspi_card_t *c, uint8_t *rx, size_t len)
{
    while (len > 0) {
        size_t n = len < sizeof(ones) ? len : sizeof(ones);

        if (c->ops->xfer(c->ops->ctx, ones, rx, n) < 0) {
            return E_BUS;
        }
        rx += n;
        len -= n;
    }
    return 0;
}

static int read_byte(const sdspi_card_t *c, uint8_t *v)
{
    return read_bytes(c, v, 1);
}

/* CS high, then 8 clocks so the card releases DO. */
static void deselect(const sdspi_card_t *c)
{
    uint8_t v;

    c->ops->cs(c->ops->ctx, 1);
    (void)read_byte(c, &v);
}

/*
 * Sends one command with CS already low and returns its R1, 0 to 0x7f,
 * or E_*. `extra` bytes after the R1 land in `r`: R3 and R7 have 4.
 */
static int command(const sdspi_card_t *c, unsigned idx, uint32_t arg, uint8_t *r, size_t extra)
{
    uint8_t f[7];
    uint8_t junk[7];
    uint8_t v = 0xff;
    unsigned i;

    f[0] = 0xff;    /* one byte of gap before the frame */
    f[1] = (uint8_t)(0x40u | idx);
    f[2] = (uint8_t)(arg >> 24);
    f[3] = (uint8_t)(arg >> 16);
    f[4] = (uint8_t)(arg >> 8);
    f[5] = (uint8_t)arg;
    f[6] = sdspi_crc7(f + 1, 5);
    if (gone(c)) {
        return E_GONE;
    }
    if (c->ops->xfer(c->ops->ctx, f, junk, sizeof(f)) < 0) {
        return E_BUS;
    }
    for (i = 0; i < SDSPI_R1_BYTES; i++) {
        if (read_byte(c, &v) < 0) {
            return E_BUS;
        }
        if (!(v & 0x80u)) {
            break;
        }
    }
    if (v & 0x80u) {
        return gone(c) ? E_GONE : E_NORESP;
    }
    if (extra > 0 && read_bytes(c, r, extra) < 0) {
        return E_BUS;
    }
    return v;
}

/* Waits for a start token, then reads len bytes and the CRC16 and checks it. */
static int data_block(const sdspi_card_t *c, uint8_t *buf, size_t len, uint32_t timeout_us)
{
    uint64_t deadline = c->ops->now_us(c->ops->ctx) + timeout_us;
    uint32_t cap = timeout_us * 4u + 64u;
    uint8_t crc[2];
    uint8_t v;

    for (;;) {
        if (read_byte(c, &v) < 0) {
            return E_BUS;
        }
        if (v == TOKEN_START) {
            break;
        }
        if (v != 0xffu) {
            if (v & 0xf0u) {
                return E_GARBAGE;
            }
            return (v & TOKEN_RANGE) ? E_TOKEN_RANGE : E_TOKEN;
        }
        if (gone(c)) {
            return E_GONE;
        }
        if (--cap == 0 || c->ops->now_us(c->ops->ctx) > deadline) {
            return E_TIMEOUT;
        }
    }
    if (read_bytes(c, buf, len) < 0 || read_bytes(c, crc, 2) < 0) {
        return E_BUS;
    }
    if (gone(c)) {
        return E_GONE;
    }
    if (sdspi_crc16(buf, len) != (uint16_t)((crc[0] << 8) | crc[1])) {
        return E_CRC;
    }
    return 0;
}

/* One command in its own transaction: CS low, command, CS high, 8 clocks. */
static int single(const sdspi_card_t *c, unsigned idx, uint32_t arg, uint8_t *r, size_t extra)
{
    int rc;

    c->ops->cs(c->ops->ctx, 0);
    rc = command(c, idx, arg, r, extra);
    deselect(c);
    return rc;
}

/* A command followed by a data block, CID or CSD, in one transaction. */
static int reg_read(const sdspi_card_t *c, unsigned idx, uint8_t *out)
{
    int rc;

    c->ops->cs(c->ops->ctx, 0);
    rc = command(c, idx, 0, NULL, 0);
    if (rc == 0) {
        rc = data_block(c, out, 16, SDSPI_TOKEN_US);
    } else if (rc > 0) {
        rc = SD_ERR_CMD;
    }
    deselect(c);
    return rc;
}

/* Init result of an internal error or an unexpected R1. */
static int init_err(int rc)
{
    if (rc == E_BUS) {
        return SD_ERR_BUS;
    }
    if (rc == E_GONE) {
        return SD_ERR_NOCARD;
    }
    return SD_ERR_CMD;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Bits lo .. lo+len-1 of a 128-bit register sent MSB first. */
static uint32_t bits(const uint8_t *r, unsigned lo, unsigned len)
{
    uint32_t v = 0;
    unsigned k;

    for (k = len; k-- > 0;) {
        unsigned b = lo + k;

        v = (v << 1) | ((r[15u - b / 8u] >> (b % 8u)) & 1u);
    }
    return v;
}

static uint64_t parse_csd(const uint8_t *r)
{
    uint32_t structure = bits(r, 126, 2);

    if (structure == 0) {
        uint64_t c_size = bits(r, 62, 12);
        uint32_t mult = bits(r, 47, 3);
        uint32_t bl_len = bits(r, 80, 4);

        return ((c_size + 1) << (mult + 2 + bl_len)) / BLK_SECTOR_SIZE;
    }
    if (structure == 1) {
        return ((uint64_t)bits(r, 48, 22) + 1) * 1024u;
    }
    return 0;
}

static void parse_cid(sd_info_t *in, const uint8_t *r)
{
    unsigned i;

    in->cid[0] = be32(r);
    in->cid[1] = be32(r + 4);
    in->cid[2] = be32(r + 8);
    in->cid[3] = be32(r + 12) & 0xffffff00u;
    in->mid = r[0];
    in->oid[0] = (char)r[1];
    in->oid[1] = (char)r[2];
    in->oid[2] = 0;
    for (i = 0; i < 5; i++) {
        in->pnm[i] = (char)r[3 + i];
    }
    in->pnm[5] = 0;
    in->prv = r[8];
    in->psn = be32(r + 9);
    in->mdt_year = (uint16_t)(2000u + bits(r, 12, 8));
    in->mdt_month = (uint8_t)bits(r, 8, 4);
}

static int go_idle(sdspi_card_t *c)
{
    const sdspi_ops_t *o = c->ops;
    uint64_t deadline = o->now_us(o->ctx) + SDSPI_CMD0_US;
    unsigned tries = SDSPI_CMD0_US / RETRY_US + 1u;
    int rc;

    for (;;) {
        rc = single(c, 0, 0, NULL, 0);
        if (rc == (int)R1_IDLE) {
            return 0;
        }
        if (rc == E_BUS) {
            return SD_ERR_BUS;
        }
        if (rc == E_GONE || --tries == 0 || o->now_us(o->ctx) > deadline) {
            return SD_ERR_NOCARD;
        }
        o->delay_us(o->ctx, RETRY_US);
    }
}

static int leave_idle(sdspi_card_t *c)
{
    const sdspi_ops_t *o = c->ops;
    uint64_t deadline = o->now_us(o->ctx) + SDSPI_INIT_US;
    unsigned tries = SDSPI_INIT_US / RETRY_US + 1u;
    int rc;

    for (;;) {
        rc = single(c, 55, 0, NULL, 0);
        if (rc < 0 || (rc & ~R1_IDLE) != 0) {
            return init_err(rc);
        }
        rc = single(c, 41, ACMD41_HCS, NULL, 0);
        if (rc == 0) {
            return 0;
        }
        if (rc != (int)R1_IDLE) {
            return init_err(rc);
        }
        if (--tries == 0 || o->now_us(o->ctx) > deadline) {
            return SD_ERR_BUSY;
        }
        o->delay_us(o->ctx, RETRY_US);
    }
}

static int identify(sdspi_card_t *c, uint32_t hz)
{
    const sdspi_ops_t *o = c->ops;
    sd_info_t *in = &c->info;
    uint8_t r[16];
    uint32_t ocr;
    int rc;

    if (o->set_hz(o->ctx, SDSPI_INIT_HZ) < 0) {
        return SD_ERR_BUS;
    }
    o->cs(o->ctx, 1);
    if (gone(c)) {
        return SD_ERR_NOCARD;
    }
    if (read_bytes(c, r, SDSPI_START_BYTES) < 0) {
        return SD_ERR_BUS;
    }
    rc = go_idle(c);
    if (rc != 0) {
        return rc;
    }

    rc = single(c, 8, 0x1aau, r, 4);
    if (rc >= 0 && (rc & R1_ILLEGAL)) {
        return SD_ERR_OLD;
    }
    if (rc != (int)R1_IDLE) {
        return init_err(rc);
    }
    if ((r[2] & 0x0fu) != 1u || r[3] != 0xaau) {
        return SD_ERR_VOLTAGE;
    }

    rc = single(c, 59, 1, NULL, 0);
    if (rc != (int)R1_IDLE) {
        return init_err(rc);
    }
    rc = leave_idle(c);
    if (rc != 0) {
        return rc;
    }

    rc = single(c, 58, 0, r, 4);
    if (rc != 0) {
        return init_err(rc);
    }
    ocr = be32(r);
    if (!(ocr & OCR_BUSY)) {
        return SD_ERR_BUSY;
    }
    if (!(ocr & OCR_33V)) {
        return SD_ERR_VOLTAGE;
    }
    in->ocr = ocr;
    in->type = (ocr & OCR_CCS) ? SD_TYPE_SDHC : SD_TYPE_SDSC;

    rc = reg_read(c, 9, r);
    if (rc != 0) {
        return init_err(rc);
    }
    in->capacity_blocks = parse_csd(r);
    if (in->capacity_blocks == 0) {
        return SD_ERR_CSD;
    }
    rc = reg_read(c, 10, r);
    if (rc != 0) {
        return init_err(rc);
    }
    parse_cid(in, r);

    if (in->type == SD_TYPE_SDSC) {
        rc = single(c, 16, BLK_SECTOR_SIZE, NULL, 0);
        if (rc != 0) {
            return init_err(rc);
        }
    }
    if (o->set_hz(o->ctx, hz) < 0) {
        return SD_ERR_BUS;
    }
    in->clock_hz = hz;
    return 0;
}

int sdspi_card_init(sdspi_card_t *c, const sdspi_ops_t *ops, uint32_t hz)
{
    int rc;

    if (c == NULL) {
        return SD_ERR_BUS;
    }
    c->ready = 0;
    c->info = (sd_info_t){0};
    c->ops = ops;
    if (ops == NULL || ops->xfer == NULL || ops->cs == NULL || ops->set_hz == NULL ||
        ops->delay_us == NULL || ops->now_us == NULL || hz == 0) {
        return SD_ERR_BUS;
    }
    rc = identify(c, hz);
    if (rc != 0) {
        c->info = (sd_info_t){0};
        return rc;
    }
    c->ready = 1;
    return 0;
}

static int read_err(int rc)
{
    switch (rc) {
    case E_GONE:        return BLK_ERR_NOMEDIA;
    case E_NORESP:      return BLK_ERR_TIMEOUT;
    case E_TIMEOUT:     return BLK_ERR_TIMEOUT;
    case E_TOKEN_RANGE: return BLK_ERR_RANGE;
    default:            break;
    }
    if (rc > 0 && (rc & (R1_ADDRESS | R1_PARAM))) {
        return BLK_ERR_RANGE;
    }
    return BLK_ERR_IO;
}

int sdspi_card_read(sdspi_card_t *c, uint64_t lba, uint8_t *buf)
{
    uint64_t addr;
    int rc;

    if (c == NULL || buf == NULL) {
        return BLK_ERR_PARAM;
    }
    if (!c->ready) {
        return BLK_ERR_NOMEDIA;
    }
    if (lba >= c->info.capacity_blocks) {
        return BLK_ERR_RANGE;
    }
    addr = c->info.type == SD_TYPE_SDHC ? lba : lba * BLK_SECTOR_SIZE;
    if (addr > 0xffffffffu) {
        return BLK_ERR_RANGE;
    }
    c->ops->cs(c->ops->ctx, 0);
    rc = command(c, 17, (uint32_t)addr, NULL, 0);
    if (rc == 0) {
        rc = data_block(c, buf, BLK_SECTOR_SIZE, SDSPI_TOKEN_US);
    }
    deselect(c);
    return rc == 0 ? 0 : read_err(rc);
}
