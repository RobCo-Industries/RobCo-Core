#ifndef SPIKES_EMMC_H
#define SPIKES_EMMC_H

#include <stdint.h>

/*
 * Polled driver for the BCM283x EMMC (Arasan SDHCI) controller.
 * Read-only: identification and single-block reads. No DMA, no interrupts.
 */

#define EMMC_BLOCK_SIZE 512

struct emmc_card {
    uint32_t host_version;  /* SLOTISR_VER bits 23:16 */
    uint32_t base_clk;      /* Hz */
    int sdhc;               /* 1: block addressing (OCR bit 30) */
    int v2;                 /* 1: card answered CMD8 */
    uint32_t ocr;
    uint32_t rca;
    uint32_t cid[4];        /* RESP3..RESP0, CRC byte stripped by the host */
};

/* Filled on failure. */
struct emmc_error {
    const char *step;
    uint32_t status;
    uint32_t interrupt;
};

/* timer_init() must run first. Returns 0 on success. */
int emmc_init(uintptr_t periph_base, uint32_t base_clk, struct emmc_card *card,
              struct emmc_error *err);

/* Reads one 512-byte block. `buf` holds 128 words. Returns 0 on success. */
int emmc_read_block(const struct emmc_card *card, uint32_t lba, uint32_t *buf,
                    struct emmc_error *err);

#endif
