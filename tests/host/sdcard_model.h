/*
 * Test-only model of an SD card in SPI mode, written from the SD Physical
 * Layer Simplified Specification and not from the driver. Tests feed it
 * every byte that reaches the card with the CS level and the clock rate,
 * and every change of the slot's pins.
 *
 * It answers like a card: SD mode until CMD0 with CS low, the idle state,
 * R1, R3 and R7 replies, start and error tokens, CRC7 on commands and
 * CRC16 on data once CMD59 turned them on (CMD0 and CMD8 always).
 *
 * It also checks the host side and counts each break as a violation,
 * keeping the first message:
 *   - a command before 74 clocks with CS high after power on,
 *   - MOSI low while CS is high or while the card sends,
 *   - CS low again, or the bus off, without 8 clocks after CS rose,
 *   - a clock over 400 kHz before ACMD41 finished,
 *   - a command with a bad CRC7 or end bit,
 *   - a write command (the driver is read-only),
 *   - power on with no card, bus on with power off, power off with the
 *     bus on, power on again less than 100 ms after it went off.
 */
#ifndef SDCARD_MODEL_H
#define SDCARD_MODEL_H

#include <stdint.h>

enum {
    SDM_SDHC = 0,   /* version 2, block addressing */
    SDM_SDSC,       /* version 2, byte addressing */
    SDM_V1,         /* version 1 SD: CMD8 is illegal */
    SDM_MMC,        /* MMC: CMD8, CMD55 and ACMD41 are illegal */
};

/* Fault hooks, bits of sdm_t.faults. */
#define SDM_F_NOANSWER    0x01u   /* MISO stays high: no card behind the socket */
#define SDM_F_BAD_ECHO    0x02u   /* CMD8 echoes a wrong check pattern */
#define SDM_F_NEVER_READY 0x04u   /* ACMD41 answers idle for ever */
#define SDM_F_BAD_DCRC    0x08u   /* CMD17 data carries a wrong CRC16 */
#define SDM_F_ERR_TOKEN   0x10u   /* CMD17 answers a data error token, card ECC failed */
#define SDM_F_NO_TOKEN    0x20u   /* CMD17 gets an R1 but never a start token */
#define SDM_F_BAD_CSD     0x40u   /* CSD_STRUCTURE 3: unknown layout */

#define SDM_OUT_MAX 640u

typedef struct {
    int kind;
    uint8_t *mem;           /* backing array, blocks * 512 bytes */
    uint32_t blocks;
    unsigned faults;
    uint64_t vanish_after;  /* non-zero: MISO all ones after this many bytes */
    int gone;               /* set once the card vanished */
    unsigned acmd41_busy;   /* ACMD41 tries answered idle before ready */
    unsigned nac;           /* 0xFF bytes before a data token */

    /* Pins. */
    int present;
    int power;
    int bus;
    uint64_t off_at;
    int ever_off;

    /* Card state. */
    int spi_mode;
    int sd_garbage;         /* got a command without the start clocks */
    int idle;
    int ready;
    int crc_on;
    int app;
    unsigned acmd41_tries;
    uint64_t start_clocks;
    int cs_low;
    int cs_was_low_on_bus;  /* CS went low since the bus went on */
    unsigned clocks_since_rise;
    uint8_t cmd[6];
    unsigned ncmd;
    uint8_t out[SDM_OUT_MAX];
    unsigned nout;
    unsigned pos;
    uint32_t lfsr;

    /* Counters for the tests. */
    uint64_t bytes;         /* bytes clocked into the powered card */
    unsigned cmds[64];
    unsigned reads;
    unsigned violations;
    char first[160];
} sdm_t;

void sdm_init(sdm_t *m, int kind, uint8_t *mem, uint32_t blocks);

/* New pin levels, after each single change, at time `now_us`. */
void sdm_pins(sdm_t *m, int present, int power, int bus, uint64_t now_us);

/* One byte on the card's side of the bus buffer. Returns the card's DO. */
uint8_t sdm_byte(sdm_t *m, int cs_low, uint8_t mosi, uint32_t hz);

/* Records a host-side break. */
void sdm_violate(sdm_t *m, const char *what);

/* The card's CID and CSD as the model sends them. */
void sdm_cid(const sdm_t *m, uint8_t out[16]);
void sdm_csd(const sdm_t *m, uint8_t out[16]);

#endif
