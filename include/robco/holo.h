#ifndef ROBCO_HOLO_H
#define ROBCO_HOLO_H

#include <stdint.h>

#include <robco/blk.h>
#include <robco/sd.h>

/*
 * Holotape slot: a removable SD card on SPI0, read-only, behind a bus
 * buffer and a switched supply. Follows the software sequence of the
 * Pip-Boy HAT spec, section 4.12. The protocol lives in sdspi.h.
 *
 * Context: main loop only. Never call any holo_* function or the block
 * device from an interrupt handler: a transfer takes milliseconds, it
 * drives SPI0 that the display shares, and nothing here is reentrant.
 *
 * SPI0 sharing rule: every SPI0 user sets the rate, mode and chip
 * select it needs before each transfer, and ends each transfer with the
 * controller idle. This driver does so, and leaves HOLO_BUS low between
 * its transfers, so the display's traffic never reaches the card.
 */

#define HOLO_HZ_DEFAULT     10000000u   /* rounds down to 9.62 MHz on a 250 MHz core */
#define HOLO_SETTLE_MS      250u        /* card detect low this long before power on */
#define HOLO_POWER_MS       10u         /* supply rise and settle after HOLO_PWR goes high */
#define HOLO_OFF_MS         100u        /* supply off at least this long before power on again */

/* holo_status() bits. */
#define HOLO_ST_CARD        0x01u   /* card detect reads low now, not debounced */
#define HOLO_ST_PRESENT     0x02u   /* card detect low for HOLO_SETTLE_MS */
#define HOLO_ST_LOCKED      0x04u   /* lock tab on; only meaningful with a card in */
#define HOLO_ST_POWER       0x08u   /* HOLO_PWR driven high */
#define HOLO_ST_BUS         0x10u   /* HOLO_BUS driven high */
#define HOLO_ST_ATTACHED    0x20u   /* the block device is live */

/* holo_poll() events. */
#define HOLO_EV_INSERTED    0x1u    /* the card became present, debounced */
#define HOLO_EV_REMOVED     0x2u    /* card detect went high: the slot is off */

typedef struct {
    uint8_t cd_pin;     /* card detect input, low with a card in */
    uint8_t wp_pin;     /* write protect input, high with the lock tab on */
    uint8_t pwr_pin;    /* supply switch output, high is on */
    uint8_t bus_pin;    /* bus buffer enable output, high connects the card */
    uint8_t cs_pin;     /* card chip select, driven as a plain GPIO output */
    uint8_t spi_cs;     /* SPI0 chip select the controller runs with, never the display's */
    uint32_t hz;        /* data rate after init */
} holo_config_t;

/* The Pip-Boy HAT: CD 12, WP 13, PWR 21, BUS 20, CS GPIO 7 on CE1. */
#define HOLO_CONFIG_DEFAULT { 12u, 13u, 21u, 20u, 7u, 1u, HOLO_HZ_DEFAULT }

/*
 * Drives HOLO_PWR and HOLO_BUS low, CS high, sets the detect pins as
 * inputs and turns MISO's pull-down off. Powers nothing. Starts SPI0 if
 * no one has. cfg NULL takes HOLO_CONFIG_DEFAULT. Returns 0 or SD_ERR_BUS.
 */
int holo_init(const holo_config_t *cfg);

/* 1 when card detect has read low for HOLO_SETTLE_MS, 0 otherwise. Never blocks. */
int holo_present(void);

/* HOLO_ST_* bits for the pins and the driver state. */
unsigned holo_status(void);

/*
 * Powers the card, waits HOLO_POWER_MS, identifies it with the bus
 * enabled, then turns the bus off and registers "holo0". Returns 0,
 * BLK_ERR_NOMEDIA with no card, SD_ERR_AGAIN while the card settles or
 * the off time runs, or another SD_ERR_*. On any error the slot is off.
 * Blocks for HOLO_POWER_MS plus the card's init, about 1 s at most.
 */
int holo_attach(void);

/* Bus low, then power low. The device stays but returns BLK_ERR_NOMEDIA. */
void holo_detach(void);

/*
 * Call every main loop pass, 100 Hz or faster. Samples card detect. A
 * high read while attached detaches at once. Returns HOLO_EV_* bits.
 */
unsigned holo_poll(void);

/* The read-only "holo0" device while attached, NULL otherwise. */
blk_dev_t *holo_blkdev(void);

/* Card details while attached, NULL otherwise. */
const sd_info_t *holo_info(void);

#endif
