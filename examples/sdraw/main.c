#include <stdint.h>

#include <robco/blk.h>
#include <robco/crc32.h>
#include <robco/klibc.h>
#include <robco/sd.h>
#include <robco/uart.h>

/*
 * Raw SD card access for tests. Commands, one per line:
 *   r <lba> <count>          read, print the CRC-32 of all bytes read
 *   w <lba> <count> <seed>   write the pattern below
 *   v <lba> <count> <seed>   read back and count bytes that differ from the pattern
 *   u <lba> <count>          read into a buffer at an odd address
 *   s                        sync
 *   i                        print the card markers again
 */

#define MAX_BLOCKS  256u
#define LINE_MAX    64

/* Spare words at both ends so `u` can use an odd address. */
static uint32_t buffer[MAX_BLOCKS * BLK_SECTOR_SIZE / 4 + 2];
static uint8_t *const buf = (uint8_t *)&buffer[1];

/*
 * Pattern: block b (absolute LBA), 32-bit word j (0..127) holds
 *   (seed * 2654435761 + b * 40503 + j * 2246822519) mod 2^32
 * stored little-endian.
 */
static void fill_pattern(uint8_t *p, uint64_t lba, uint32_t count, uint32_t seed)
{
    uint32_t k;
    uint32_t j;

    for (k = 0; k < count; k++) {
        uint32_t b = (uint32_t)(lba + k);

        for (j = 0; j < BLK_SECTOR_SIZE / 4; j++) {
            uint32_t v = seed * 2654435761u + b * 40503u + j * 2246822519u;

            *p++ = (uint8_t)v;
            *p++ = (uint8_t)(v >> 8);
            *p++ = (uint8_t)(v >> 16);
            *p++ = (uint8_t)(v >> 24);
        }
    }
}

static void print_card(int rc)
{
    const sd_info_t *in = sd_info();

    if (in == NULL) {
        k_printf("@RC stage=sdraw ev=card rc=%s type=none blocks=0 clk=0\n", sd_strerror(rc));
        return;
    }
    k_printf("@RC stage=sdraw ev=card rc=%s type=%s blocks=%u clk=%u\n", sd_strerror(rc),
             in->type == SD_TYPE_SDHC ? "sdhc" : "sdsc", (unsigned)in->capacity_blocks,
             (unsigned)in->clock_hz);
    k_printf("@RC stage=sdraw ev=info ocr=0x%08x rca=0x%04x mid=0x%02x oid=%s pnm=%s "
             "prv=0x%02x psn=0x%08x mdt=%u-%02u cid=%08x%08x%08x%08x\n",
             (unsigned)in->ocr, (unsigned)in->rca, (unsigned)in->mid, in->oid, in->pnm,
             (unsigned)in->prv, (unsigned)in->psn, (unsigned)in->mdt_year,
             (unsigned)in->mdt_month, (unsigned)in->cid[0], (unsigned)in->cid[1],
             (unsigned)in->cid[2], (unsigned)in->cid[3]);
    /* One run on a real board tells whether its slot reports card detect. */
    k_printf("@RC stage=sdraw ev=detect present=0x%08x inserted=%u card_detect=%s\n",
             (unsigned)in->present_state, (unsigned)(in->present_state >> 16) & 1u,
             in->card_detect ? "reported" : "not_reported");
}

static void print_mbr(void)
{
    const uint8_t *p0 = buf + 446;
    int rc = blk_read(sd_blkdev(), 0, 1, buf);
    uint32_t lba;

    if (rc != 0) {
        k_printf("@RC stage=sdraw ev=mbr rc=%s\n", blk_strerror(rc));
        return;
    }
    lba = (uint32_t)p0[8] | ((uint32_t)p0[9] << 8) | ((uint32_t)p0[10] << 16) |
          ((uint32_t)p0[11] << 24);
    k_printf("@RC stage=sdraw ev=mbr rc=ok sig=0x%02x%02x p0_type=0x%02x p0_lba=%u crc=0x%08x\n",
             (unsigned)buf[510], (unsigned)buf[511], (unsigned)p0[4], (unsigned)lba,
             (unsigned)crc32(buf, BLK_SECTOR_SIZE));
}

static int read_line(char *line)
{
    int n = 0;

    for (;;) {
        int c = uart_getc();

        if (c == '\r' || c == '\n') {
            if (n > 0) {
                line[n] = 0;
                return n;
            }
            continue;
        }
        if (n < LINE_MAX - 1) {
            line[n++] = (char)c;
        }
    }
}

/* Parses up to `max` unsigned decimal numbers after the command letter. */
static int parse_args(const char *s, uint64_t *out, int max)
{
    int n = 0;

    s++;
    while (n < max) {
        uint64_t v = 0;

        while (*s == ' ') {
            s++;
        }
        if (*s < '0' || *s > '9') {
            break;
        }
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint64_t)(*s++ - '0');
        }
        out[n++] = v;
    }
    return n;
}

/* Runs a block transfer through the checked blk.h wrappers. */
static int transfer(int write, uint64_t lba, uint64_t count, uint8_t *p)
{
    blk_dev_t *dev = sd_blkdev();

    if (dev == NULL) {
        return BLK_ERR_NOMEDIA;
    }
    if (count > MAX_BLOCKS) {
        return BLK_ERR_PARAM;
    }
    return write ? blk_write(dev, lba, (uint32_t)count, p)
                 : blk_read(dev, lba, (uint32_t)count, p);
}

static void command(const char *line, int init_rc)
{
    uint64_t a[3] = {0, 0, 0};
    int argc = parse_args(line, a, 3);
    unsigned lba = (unsigned)a[0];
    unsigned count = (unsigned)a[1];
    uint32_t crc = 0;
    int rc;

    switch (line[0]) {
    case 'r':
    case 'u': {
        uint8_t *p = line[0] == 'u' ? buf + 1 : buf;

        rc = argc == 2 ? transfer(0, a[0], a[1], p) : BLK_ERR_PARAM;
        if (rc == 0) {
            crc = crc32(p, (size_t)count * BLK_SECTOR_SIZE);
        }
        k_printf("@RC stage=sdraw ev=%s lba=%u count=%u rc=%s crc=0x%08x\n",
                 line[0] == 'u' ? "uread" : "read", lba, count, blk_strerror(rc),
                 (unsigned)crc);
        break;
    }
    case 'w':
        rc = BLK_ERR_PARAM;
        if (argc == 3 && a[1] <= MAX_BLOCKS) {
            fill_pattern(buf, a[0], (uint32_t)a[1], (uint32_t)a[2]);
            rc = transfer(1, a[0], a[1], buf);
        }
        k_printf("@RC stage=sdraw ev=write lba=%u count=%u rc=%s\n", lba, count,
                 blk_strerror(rc));
        break;
    case 'v': {
        static uint8_t want[BLK_SECTOR_SIZE];
        unsigned mismatches = 0;
        uint32_t k;
        uint32_t i;

        rc = argc == 3 ? transfer(0, a[0], a[1], buf) : BLK_ERR_PARAM;
        for (k = 0; rc == 0 && k < count; k++) {
            fill_pattern(want, a[0] + k, 1, (uint32_t)a[2]);
            for (i = 0; i < BLK_SECTOR_SIZE; i++) {
                mismatches += buf[k * BLK_SECTOR_SIZE + i] != want[i];
            }
        }
        k_printf("@RC stage=sdraw ev=verify lba=%u count=%u rc=%s mismatches=%u\n", lba, count,
                 blk_strerror(rc), mismatches);
        break;
    }
    case 's':
        rc = sd_blkdev() != NULL ? blk_sync(sd_blkdev()) : BLK_ERR_NOMEDIA;
        k_printf("@RC stage=sdraw ev=sync rc=%s\n", blk_strerror(rc));
        break;
    case 'i':
        print_card(init_rc);
        break;
    default:
        k_printf("@RC stage=sdraw ev=error what=unknown_command\n");
        break;
    }
}

void robco_main(void)
{
    char line[LINE_MAX];
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=sdraw ev=boot\n");

    rc = sd_init();
    print_card(rc);
    if (rc == 0) {
        print_mbr();
    }

    k_printf("@RC stage=sdraw ev=ready\n");
    for (;;) {
        read_line(line);
        command(line, rc);
    }
}
