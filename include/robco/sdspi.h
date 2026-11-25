#ifndef ROBCO_SDSPI_H
#define ROBCO_SDSPI_H

#include <stddef.h>
#include <stdint.h>

#include <robco/blk.h>
#include <robco/sd.h>

/*
 * SD card protocol in SPI mode, read-only, one block per command. Pure
 * code: it reaches the card only through the functions in sdspi_ops_t,
 * so host tests run it against a card model. holo.h wraps it for the
 * holotape slot. Main context only, never from an interrupt handler.
 *
 * Supported: SD version 2 cards, SDSC and SDHC or SDXC, at 3.3 V.
 * Refused with SD_ERR_OLD: version 1 SD cards and MMC cards.
 */

#define SDSPI_INIT_HZ       400000u     /* identification rate, the SD maximum */
#define SDSPI_START_BYTES   10u         /* 80 clocks with CS high before CMD0 */

/* Every wait has a deadline, in microseconds. */
#define SDSPI_CMD0_US       100000u     /* CMD0 tries until the card answers idle */
#define SDSPI_R1_BYTES      9u          /* bytes polled for an R1, Ncr is 8 at most */
#define SDSPI_INIT_US       1000000u    /* ACMD41 until the card leaves idle */
#define SDSPI_TOKEN_US      100000u     /* a data start token after CMD9, CMD10 or CMD17 */
#define SDSPI_RETRY_US      1000u       /* pause between CMD0 or ACMD41 tries */
#define SDSPI_NCX_BYTES     16u         /* bytes polled for the CSD or CID token, Ncx is 8 at most */

/*
 * Bytes one init step clocks, at most: a CMD9 or CMD10 with its R1 poll,
 * its token poll, 16 bytes, the CRC and the 8 trailing clocks is 51. At
 * SDSPI_INIT_HZ that is about 1.3 ms.
 */
#define SDSPI_STEP_BYTES    64u

typedef struct {
    void *ctx;
    /* Clocks len bytes. tx is never NULL. Returns 0, or negative on a bus failure. */
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    /* Drives the card's chip select: 1 high, 0 low. */
    void (*cs)(void *ctx, int high);
    /* Sets the clock for the next transfers. Returns 0 or negative. */
    int (*set_hz)(void *ctx, uint32_t hz);
    void (*delay_us)(void *ctx, uint32_t us);
    uint64_t (*now_us)(void *ctx);
    /* Optional. Returns 0 once the card is known gone: every wait stops at once. */
    int (*present)(void *ctx);
} sdspi_ops_t;

typedef struct {
    const sdspi_ops_t *ops;
    sd_info_t info;     /* rca is 0: SPI mode has no relative address */
    int ready;
    /* Stepped init state. Private. */
    int phase;
    int result;
    uint32_t hz;
    unsigned tries;
    uint64_t deadline;
    uint64_t wake_us;
} sdspi_card_t;

/*
 * Runs the whole identification: 80 clocks with CS high, CMD0, CMD8 and
 * its echo, CMD59 to turn CRC checks on, ACMD41 with HCS, CMD58, CMD9,
 * CMD10 and CMD16 for SDSC, all at SDSPI_INIT_HZ, then switches to
 * `hz`. Ends with CS high and 8 more clocks. Returns 0 or SD_ERR_*.
 */
int sdspi_card_init(sdspi_card_t *c, const sdspi_ops_t *ops, uint32_t hz);

/*
 * The same init in steps, for a caller that cannot block. sdspi_card_init()
 * is a loop over these. The start clocks nothing and returns SD_PENDING,
 * or SD_ERR_BUS for bad arguments.
 *
 * Each step clocks one command and its reply, SDSPI_STEP_BYTES at most,
 * and ends with CS high and 8 more clocks, so the caller may give the bus
 * to someone else between two steps. A step sets SDSPI_INIT_HZ before it
 * clocks, and the last one sets `hz`. The pairs that need each other stay
 * in one step: the start clocks and the first CMD0, CMD55 and its ACMD41.
 *
 * A step returns SD_PENDING while work remains, 0 when the card is ready,
 * or SD_ERR_*. After a final result it returns that result again.
 */
int sdspi_init_start(sdspi_card_t *c, const sdspi_ops_t *ops, uint32_t hz);
int sdspi_init_step(sdspi_card_t *c);

/*
 * Microseconds until the next step has work: the pause between two CMD0
 * or two ACMD41 tries. 0 when a step should run now. A step called sooner
 * still runs, and spends one of the tries.
 */
uint32_t sdspi_init_wait_us(const sdspi_card_t *c);

/*
 * Reads block `lba` into buf, 512 bytes, any alignment, with CMD17. Checks
 * the data CRC16. Ends with CS high and 8 more clocks. Returns 0 or
 * BLK_ERR_*: BLK_ERR_NOMEDIA before init or when present() says gone,
 * BLK_ERR_TIMEOUT when no reply or token comes, BLK_ERR_RANGE for an
 * address error, BLK_ERR_IO for anything else.
 */
int sdspi_card_read(sdspi_card_t *c, uint64_t lba, uint8_t *buf);

/* CRC7 of a command, already shifted with the end bit: the sixth byte. */
uint8_t sdspi_crc7(const uint8_t *p, size_t len);

/* CRC16-CCITT, polynomial 0x1021, initial 0, as on SD data blocks. */
uint16_t sdspi_crc16(const uint8_t *p, size_t len);

#endif
