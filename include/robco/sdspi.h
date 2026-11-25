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
} sdspi_card_t;

/*
 * Runs the whole identification: 80 clocks with CS high, CMD0, CMD8 and
 * its echo, CMD59 to turn CRC checks on, ACMD41 with HCS, CMD58, CMD9,
 * CMD10 and CMD16 for SDSC, all at SDSPI_INIT_HZ, then switches to
 * `hz`. Ends with CS high and 8 more clocks. Returns 0 or SD_ERR_*.
 */
int sdspi_card_init(sdspi_card_t *c, const sdspi_ops_t *ops, uint32_t hz);

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
