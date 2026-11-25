#include <stdint.h>

#include <robco/blk.h>
#include <robco/crc32.h>
#include <robco/gpio.h>
#include <robco/holo.h>
#include <robco/klibc.h>
#include <robco/sd.h>
#include <robco/timer.h>
#include <robco/uart.h>

/*
 * Holotape slot bring-up tool. Commands, one per line:
 *   p                  show the slot's pins and state
 *   a                  attach: power, init, register holo0
 *   i                  print the card's identity, no serial number
 *   s                  print the identity with the serial number and raw CID
 *   r <lba> <count>    read blocks, print the CRC-32 of all bytes and the time
 *   d                  detach: bus off, power off
 * The main loop polls card detect and reports insertion and removal as
 * they happen. A removal turns the slot off at once.
 */

#define MAX_BLOCKS  64u
#define LINE_MAX    64
#define ATTACH_WAIT_MS  1000u   /* attach retries SD_ERR_AGAIN this long */

static uint8_t buf[MAX_BLOCKS * BLK_SECTOR_SIZE];

static void print_pins(void)
{
    const holo_config_t c = HOLO_CONFIG_DEFAULT;
    unsigned st = holo_status();

    k_printf("@RC stage=holo ev=pins cd=%d wp=%d pwr=%d bus=%d cs=%d card=%u present=%u "
             "locked=%u attached=%u\n",
             gpio_read(c.cd_pin), gpio_read(c.wp_pin), gpio_read(c.pwr_pin),
             gpio_read(c.bus_pin), gpio_read(c.cs_pin), (st & HOLO_ST_CARD) != 0,
             (st & HOLO_ST_PRESENT) != 0, (st & HOLO_ST_LOCKED) != 0,
             (st & HOLO_ST_ATTACHED) != 0);
}

static void print_card(int serial)
{
    const sd_info_t *in = holo_info();

    if (in == NULL) {
        k_printf("@RC stage=holo ev=card type=none\n");
        return;
    }
    k_printf("@RC stage=holo ev=card type=%s blocks=%u clk=%u ocr=0x%08x mid=0x%02x oid=%s "
             "pnm=%s prv=%u.%u mdt=%u-%02u\n",
             in->type == SD_TYPE_SDHC ? "sdhc" : "sdsc", (unsigned)in->capacity_blocks,
             (unsigned)in->clock_hz, (unsigned)in->ocr, (unsigned)in->mid, in->oid, in->pnm,
             (unsigned)(in->prv >> 4), (unsigned)(in->prv & 15u), (unsigned)in->mdt_year,
             (unsigned)in->mdt_month);
    if (serial) {
        k_printf("@RC stage=holo ev=serial psn=0x%08x cid=%08x%08x%08x%08x\n",
                 (unsigned)in->psn, (unsigned)in->cid[0], (unsigned)in->cid[1],
                 (unsigned)in->cid[2], (unsigned)in->cid[3]);
    }
}

static void report(unsigned ev)
{
    if (ev & HOLO_EV_INSERTED) {
        k_printf("@RC stage=holo ev=inserted\n");
    }
    if (ev & HOLO_EV_REMOVED) {
        k_printf("@RC stage=holo ev=removed\n");
    }
}

static void attach(void)
{
    uint64_t deadline = timer_now_us() + ATTACH_WAIT_MS * 1000ull;
    uint64_t t0 = timer_now_us();
    int rc;

    for (;;) {
        report(holo_poll());
        rc = holo_attach();
        if (rc != SD_ERR_AGAIN || timer_expired(deadline)) {
            break;
        }
        timer_delay_ms(10);
    }
    k_printf("@RC stage=holo ev=attach rc=%s ms=%u\n", sd_strerror(rc),
             (unsigned)((timer_now_us() - t0) / 1000u));
    if (rc == 0) {
        print_card(0);
    }
}

/* Parses up to `max` unsigned decimal numbers after the command letter. */
static int parse_args(const char *s, uint32_t *out, int max)
{
    int n = 0;

    s++;
    while (n < max) {
        uint32_t v = 0;

        while (*s == ' ') {
            s++;
        }
        if (*s < '0' || *s > '9') {
            break;
        }
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (uint32_t)(*s++ - '0');
        }
        out[n++] = v;
    }
    return n;
}

static void read_blocks(const char *line)
{
    uint32_t a[2] = {0, 0};
    blk_dev_t *dev = holo_blkdev();
    uint32_t crc = 0;
    uint64_t t0;
    int rc;

    if (parse_args(line, a, 2) != 2 || a[1] == 0 || a[1] > MAX_BLOCKS) {
        rc = BLK_ERR_PARAM;
    } else if (dev == NULL) {
        rc = BLK_ERR_NOMEDIA;
    } else {
        rc = 0;
    }
    t0 = timer_now_us();
    if (rc == 0) {
        rc = blk_read(dev, a[0], a[1], buf);
    }
    if (rc == 0) {
        crc = crc32(buf, (size_t)a[1] * BLK_SECTOR_SIZE);
    }
    k_printf("@RC stage=holo ev=read lba=%u count=%u rc=%s crc=0x%08x us=%u\n", (unsigned)a[0],
             (unsigned)a[1], blk_strerror(rc), (unsigned)crc,
             (unsigned)(timer_now_us() - t0));
}

static void command(const char *line)
{
    switch (line[0]) {
    case 'p':
        print_pins();
        break;
    case 'a':
        attach();
        break;
    case 'i':
        print_card(0);
        break;
    case 's':
        print_card(1);
        break;
    case 'r':
        read_blocks(line);
        break;
    case 'd':
        holo_detach();
        k_printf("@RC stage=holo ev=detach\n");
        break;
    default:
        k_printf("@RC stage=holo ev=error what=unknown_command\n");
        break;
    }
}

void robco_main(void)
{
    char line[LINE_MAX];
    int n = 0;
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=holo ev=boot\n");
    rc = holo_init(NULL);
    k_printf("@RC stage=holo ev=init rc=%s\n", sd_strerror(rc));
    print_pins();
    k_printf("@RC stage=holo ev=ready\n");
    for (;;) {
        int c = uart_poll();

        report(holo_poll());
        if (c < 0) {
            timer_delay_ms(1);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n > 0) {
                line[n] = 0;
                command(line);
                n = 0;
            }
            continue;
        }
        if (n < LINE_MAX - 1) {
            line[n++] = (char)c;
        }
    }
}
