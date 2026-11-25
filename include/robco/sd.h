#ifndef ROBCO_SD_H
#define ROBCO_SD_H

#include <stdint.h>

#include <robco/blk.h>

/*
 * SD card on the BCM283x EMMC (Arasan SDHCI) controller. Polled PIO,
 * 1-bit bus, 25 MHz. No DMA, no interrupts. Main context only.
 */

/* sd_init() results. Block transfers return BLK_ERR_* codes instead. */
#define SD_ERR_BOARD    (-10)   /* unknown board, or BCM2711 */
#define SD_ERR_CLOCK    (-11)   /* no base clock, or the SD clock did not settle */
#define SD_ERR_RESET    (-12)   /* the controller did not leave reset */
#define SD_ERR_NOCARD   (-13)   /* no answer to CMD8 nor to ACMD41 */
#define SD_ERR_VOLTAGE  (-14)   /* CMD8 echo wrong, or the card rejects 3.3 V */
#define SD_ERR_BUSY     (-15)   /* the card stayed busy in ACMD41 */
#define SD_ERR_CMD      (-16)   /* an identification command failed */
#define SD_ERR_CSD      (-17)   /* unknown CSD layout */
#define SD_ERR_OLD      (-18)   /* version 1 SD or MMC card: not supported over SPI */
#define SD_ERR_AGAIN    (-19)   /* holotape slot: card not settled or off time not over */
#define SD_ERR_BUS      (-20)   /* holotape slot: SPI0 or GPIO failed, or bad config */

typedef enum {
    SD_TYPE_NONE = 0,
    SD_TYPE_SDSC = 1,   /* byte addressing */
    SD_TYPE_SDHC = 2,   /* block addressing, SDHC and SDXC */
} sd_type_t;

typedef struct {
    sd_type_t type;
    uint32_t ocr;
    uint32_t rca;
    uint32_t cid[4];            /* raw CID bits 127:8, cid[0] holds bits 127:96 */
    uint8_t mid;                /* manufacturer ID */
    char oid[3];                /* OEM ID, 2 chars */
    char pnm[6];                /* product name, 5 chars */
    uint8_t prv;                /* product revision, BCD n.m */
    uint32_t psn;               /* serial number */
    uint16_t mdt_year;          /* manufacturing date */
    uint8_t mdt_month;
    uint64_t capacity_blocks;   /* 512-byte blocks, from the CSD */
    uint32_t clock_hz;          /* bus clock in use */
} sd_info_t;

/*
 * Moves the card to EMMC, resets the controller and identifies the card.
 * Returns 0 or SD_ERR_*. Calling it again restarts from scratch.
 */
int sd_init(void);

/* Name of an SD_ERR_* or BLK_ERR_* code, for example "ok" or "nocard". */
const char *sd_strerror(int err);

/* The "sd0" block device after a successful sd_init(), NULL otherwise. */
blk_dev_t *sd_blkdev(void);

/* Card details after a successful sd_init(), NULL otherwise. */
const sd_info_t *sd_info(void);

/* Stops the SD clock and powers the bus off. sd_blkdev() returns NULL after it. */
void sd_deinit(void);

#endif
