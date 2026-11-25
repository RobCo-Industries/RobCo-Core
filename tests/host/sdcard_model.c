/*
 * SD card model in SPI mode for host tests. See sdcard_model.h. The CRC
 * code here is written apart from src/lib/sdspi.c on purpose, table-free
 * and bit by bit from the polynomials, so one cannot hide a bug in the
 * other.
 */
#include <stdio.h>
#include <string.h>

#include "sdcard_model.h"

#define INIT_HZ_MAX 400000u
#define START_CLOCKS 74u
#define OFF_US_MIN 100000u

void sdm_violate(sdm_t *m, const char *what)
{
    if (m->violations++ == 0) {
        snprintf(m->first, sizeof(m->first), "%s", what);
    }
}

/* x^7 + x^3 + 1 over the bits, MSB first. */
static uint8_t crc7(const uint8_t *p, unsigned n)
{
    unsigned reg = 0;
    unsigned i;
    int b;

    for (i = 0; i < n; i++) {
        for (b = 7; b >= 0; b--) {
            unsigned in = (p[i] >> b) & 1u;
            unsigned top = (reg >> 6) & 1u;

            reg = (reg << 1) & 0x7fu;
            if (in ^ top) {
                reg ^= 0x09u;
            }
        }
    }
    return (uint8_t)reg;
}

/* x^16 + x^12 + x^5 + 1, MSB first, initial 0. */
static uint16_t crc16(const uint8_t *p, unsigned n)
{
    unsigned reg = 0;
    unsigned i;
    int b;

    for (i = 0; i < n; i++) {
        for (b = 7; b >= 0; b--) {
            unsigned in = (p[i] >> b) & 1u;
            unsigned top = (reg >> 15) & 1u;

            reg = (reg << 1) & 0xffffu;
            if (in ^ top) {
                reg ^= 0x1021u;
            }
        }
    }
    return (uint16_t)reg;
}

static void set_bits(uint8_t r[16], unsigned lo, unsigned len, uint32_t v)
{
    unsigned k;

    for (k = 0; k < len; k++) {
        unsigned b = lo + k;
        uint8_t mask = (uint8_t)(1u << (b % 8u));

        if ((v >> k) & 1u) {
            r[15u - b / 8u] |= mask;
        } else {
            r[15u - b / 8u] &= (uint8_t)~mask;
        }
    }
}

static void seal(uint8_t r[16])
{
    r[15] = (uint8_t)((crc7(r, 15) << 1) | 1u);
}

void sdm_cid(const sdm_t *m, uint8_t r[16])
{
    memset(r, 0, 16);
    r[0] = 0x03;                    /* MID */
    r[1] = 'S';                     /* OID */
    r[2] = 'D';
    memcpy(r + 3, m->kind == SDM_SDSC ? "TAPE2" : "TAPE1", 5);  /* PNM */
    r[8] = 0x42;                    /* PRV 4.2 */
    r[9] = 0xde;                    /* PSN */
    r[10] = 0xad;
    r[11] = 0xbe;
    r[12] = 0xef;
    set_bits(r, 12, 8, 24);         /* MDT year 2024 */
    set_bits(r, 8, 4, 6);           /* MDT month 6 */
    seal(r);
}

void sdm_csd(const sdm_t *m, uint8_t r[16])
{
    memset(r, 0, 16);
    if (m->faults & SDM_F_BAD_CSD) {
        set_bits(r, 126, 2, 3);
    } else if (m->kind == SDM_SDHC) {
        set_bits(r, 126, 2, 1);
        set_bits(r, 80, 4, 9);
        set_bits(r, 48, 22, m->blocks / 1024u - 1u);
    } else {
        /* blocks = (C_SIZE + 1) << (C_SIZE_MULT + 2), with READ_BL_LEN 9. */
        unsigned mult = 0;

        while ((m->blocks >> (mult + 2u)) > 4096u && mult < 7u) {
            mult++;
        }
        set_bits(r, 126, 2, 0);
        set_bits(r, 80, 4, 9);
        set_bits(r, 62, 12, (m->blocks >> (mult + 2u)) - 1u);
        set_bits(r, 47, 3, mult);
    }
    seal(r);
}

static void power_on_reset(sdm_t *m)
{
    m->spi_mode = 0;
    m->sd_garbage = 0;
    m->idle = 0;
    m->ready = 0;
    m->crc_on = 0;
    m->app = 0;
    m->acmd41_tries = 0;
    m->start_clocks = 0;
    m->cs_low = 0;
    m->cs_was_low_on_bus = 0;
    m->clocks_since_rise = 1;
    m->ncmd = 0;
    m->nout = m->pos = 0;
}

void sdm_init(sdm_t *m, int kind, uint8_t *mem, uint32_t blocks)
{
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    m->mem = mem;
    m->blocks = blocks;
    m->acmd41_busy = 3;
    m->nac = 2;
    m->present = 1;
    m->lfsr = 0xace1u;
    power_on_reset(m);
}

void sdm_pins(sdm_t *m, int present, int power, int bus, uint64_t now_us)
{
    if (power && !m->power) {
        if (!present) {
            sdm_violate(m, "power on with no card");
        }
        if (m->ever_off && now_us - m->off_at < OFF_US_MIN) {
            sdm_violate(m, "power on again less than 100 ms after off");
        }
        power_on_reset(m);
    }
    if (!power && m->power) {
        if (bus) {
            sdm_violate(m, "power off with the bus on");
        }
        m->off_at = now_us;
        m->ever_off = 1;
    }
    if (bus && !m->bus) {
        if (!power) {
            sdm_violate(m, "bus on with the power off");
        }
        m->cs_was_low_on_bus = 0;
    }
    if (!bus && m->bus && present && power && !m->gone) {
        if (m->cs_low) {
            sdm_violate(m, "bus off with CS low");
        } else if (m->cs_was_low_on_bus && m->clocks_since_rise == 0) {
            sdm_violate(m, "bus off without 8 clocks after CS rose");
        }
    }
    if (!present && m->present) {
        m->gone = 1;    /* a pulled card answers nothing */
    }
    if (present && !m->present) {
        m->gone = 0;    /* a new card: it waits for a power cycle */
        m->spi_mode = 0;
        m->sd_garbage = 0;
    }
    m->present = present;
    m->power = power;
    m->bus = bus;
}

static void queue(sdm_t *m, uint8_t v)
{
    if (m->nout < SDM_OUT_MAX) {
        m->out[m->nout++] = v;
    }
}

static uint8_t r1(const sdm_t *m, uint8_t flags)
{
    return (uint8_t)((m->idle ? 0x01u : 0x00u) | flags);
}

static void data(sdm_t *m, const uint8_t *p, unsigned n, int bad_crc)
{
    uint16_t c = crc16(p, n);
    unsigned i;

    for (i = 0; i < m->nac; i++) {
        queue(m, 0xff);
    }
    queue(m, 0xfe);
    for (i = 0; i < n; i++) {
        queue(m, p[i]);
    }
    if (bad_crc) {
        c ^= 0x0100u;
    }
    queue(m, (uint8_t)(c >> 8));
    queue(m, (uint8_t)c);
}

static void do_command(sdm_t *m)
{
    unsigned idx = m->cmd[0] & 0x3fu;
    uint32_t arg = ((uint32_t)m->cmd[1] << 24) | ((uint32_t)m->cmd[2] << 16) |
                   ((uint32_t)m->cmd[3] << 8) | m->cmd[4];
    int app = m->app;
    uint8_t reg[16];
    char msg[96];

    m->nout = m->pos = 0;
    m->app = 0;
    m->cmds[idx]++;
    queue(m, 0xff);     /* Ncr: one byte before the reply */

    if (!(m->cmd[5] & 1u)) {
        snprintf(msg, sizeof(msg), "CMD%u without its end bit", idx);
        sdm_violate(m, msg);
    }
    if ((m->crc_on || idx == 0 || idx == 8) && (m->cmd[5] >> 1) != crc7(m->cmd, 5)) {
        snprintf(msg, sizeof(msg), "CMD%u with a bad CRC7", idx);
        sdm_violate(m, msg);
        queue(m, r1(m, 0x08));
        return;
    }
    if (idx == 24 || idx == 25 || idx == 27 || idx == 32 || idx == 33 || idx == 38) {
        snprintf(msg, sizeof(msg), "write or erase command CMD%u", idx);
        sdm_violate(m, msg);
        queue(m, r1(m, 0x04));
        return;
    }
    if (m->kind == SDM_MMC && (idx == 8 || idx == 55 || idx == 41)) {
        queue(m, r1(m, 0x04));
        return;
    }
    switch (idx) {
    case 0:
        m->idle = 1;
        m->ready = 0;
        m->acmd41_tries = 0;
        queue(m, 0x01);
        break;
    case 8:
        if (m->kind == SDM_V1) {
            queue(m, r1(m, 0x04));
            break;
        }
        queue(m, r1(m, 0));
        queue(m, 0x00);
        queue(m, 0x00);
        queue(m, (uint8_t)((arg >> 8) & 0x0fu));
        queue(m, (uint8_t)((m->faults & SDM_F_BAD_ECHO) ? (arg ^ 0x55u) : arg));
        break;
    case 59:
        m->crc_on = (int)(arg & 1u);
        queue(m, r1(m, 0));
        break;
    case 55:
        m->app = 1;
        queue(m, r1(m, 0));
        break;
    case 41:
        if (!app) {
            queue(m, r1(m, 0x04));
            break;
        }
        m->acmd41_tries++;
        if (!(m->faults & SDM_F_NEVER_READY) && m->acmd41_tries > m->acmd41_busy &&
            (m->kind != SDM_SDHC || (arg & 0x40000000u))) {
            m->idle = 0;
            m->ready = 1;
        }
        queue(m, r1(m, 0));
        break;
    case 58: {
        uint32_t ocr = 0x00ff8000u;

        if (m->ready) {
            ocr |= 0x80000000u;
            if (m->kind == SDM_SDHC) {
                ocr |= 0x40000000u;
            }
        }
        queue(m, r1(m, 0));
        queue(m, (uint8_t)(ocr >> 24));
        queue(m, (uint8_t)(ocr >> 16));
        queue(m, (uint8_t)(ocr >> 8));
        queue(m, (uint8_t)ocr);
        break;
    }
    case 9:
    case 10:
        if (m->idle) {
            queue(m, r1(m, 0x04));
            break;
        }
        queue(m, 0x00);
        if (idx == 9) {
            sdm_csd(m, reg);
        } else {
            sdm_cid(m, reg);
        }
        data(m, reg, 16, 0);
        break;
    case 16:
        queue(m, r1(m, arg == 512u ? 0 : 0x40));
        break;
    case 17: {
        uint32_t blk = arg;

        if (m->idle) {
            queue(m, r1(m, 0x04));
            break;
        }
        if (m->kind == SDM_SDSC) {
            if (arg % 512u) {
                queue(m, 0x20);
                break;
            }
            blk = arg / 512u;
        }
        queue(m, 0x00);
        if (m->faults & SDM_F_NO_TOKEN) {
            break;
        }
        if (blk >= m->blocks) {
            queue(m, 0xff);
            queue(m, 0x08);     /* error token: out of range */
            break;
        }
        if (m->faults & SDM_F_ERR_TOKEN) {
            queue(m, 0xff);
            queue(m, 0x04);     /* error token: card ECC failed */
            break;
        }
        m->reads++;
        data(m, m->mem + (size_t)blk * 512u, 512, (m->faults & SDM_F_BAD_DCRC) != 0);
        break;
    }
    default:
        queue(m, r1(m, 0x04));
        break;
    }
}

static uint8_t garbage(sdm_t *m)
{
    uint8_t v;

    m->lfsr = m->lfsr * 1103515245u + 12345u;
    v = (uint8_t)(m->lfsr >> 16);
    return v == 0x01u || v == 0xffu ? 0x5a : v;
}

uint8_t sdm_byte(sdm_t *m, int cs_low, uint8_t mosi, uint32_t hz)
{
    uint8_t out = 0xff;

    if (!m->power || !m->present) {
        if (!m->present && m->power) {
            sdm_violate(m, "clocks into an empty socket with power on");
        }
        return 0xff;
    }
    m->bytes++;
    if (m->vanish_after != 0 && m->bytes > m->vanish_after) {
        m->gone = 1;
    }
    if (m->gone) {
        return 0xff;
    }
    if (!m->ready && hz > INIT_HZ_MAX) {
        sdm_violate(m, "clock over 400 kHz before ACMD41 finished");
    }

    if (cs_low && !m->cs_low) {
        if (m->clocks_since_rise == 0) {
            sdm_violate(m, "CS low again without 8 clocks after it rose");
        }
        if (!m->spi_mode && m->start_clocks < START_CLOCKS) {
            sdm_violate(m, "command before 74 clocks with CS high");
            m->sd_garbage = 1;
        }
        m->cs_was_low_on_bus = 1;
    }
    if (!cs_low && m->cs_low) {
        m->clocks_since_rise = 0;
        m->nout = m->pos = 0;
        m->ncmd = 0;
    }
    m->cs_low = cs_low;

    if (!cs_low) {
        if (mosi != 0xff) {
            sdm_violate(m, "MOSI low while CS is high");
        }
        m->clocks_since_rise++;
        if (!m->spi_mode) {
            m->start_clocks += 8;
        }
        return 0xff;
    }
    if (m->sd_garbage) {
        return garbage(m);
    }
    if (m->pos < m->nout) {
        if (mosi != 0xff) {
            sdm_violate(m, "MOSI low while the card sends");
        }
        return m->out[m->pos++];
    }
    if (m->ncmd == 0) {
        if (mosi == 0xff) {
            return 0xff;
        }
        if ((mosi & 0xc0u) != 0x40u) {
            sdm_violate(m, "a command byte without its start bits");
            return 0xff;
        }
    }
    m->cmd[m->ncmd++] = mosi;
    if (m->ncmd == 6) {
        m->ncmd = 0;
        if (!m->spi_mode) {
            if ((m->cmd[0] & 0x3fu) != 0) {
                /* SD mode: only CMD0 with CS low moves the card to SPI. */
                return out;
            }
            m->spi_mode = 1;
        }
        do_command(m);
    }
    return out;
}
