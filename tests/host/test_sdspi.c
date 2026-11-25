/*
 * The SD-over-SPI protocol of src/lib/sdspi.c against the card model in
 * sdcard_model.c. The ops here wire the protocol straight to the model:
 * CS and every byte go to the card, each byte costs 10 us of fake time.
 * The slot layer and SPI0 are tested in test_holo.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <robco/blk.h>
#include <robco/sd.h>
#include <robco/sdspi.h>

#include "rc_test.h"
#include "sdcard_model.c"

#define BLOCKS      4096u           /* a 2 MiB tape */
#define BYTE_NS     10000u
#define RUN_HZ      9615384u

static sdm_t card;
static uint8_t *mem;
static uint64_t now_ns;
static uint32_t hz;
static int cs_high = 1;
static unsigned xfer_fail_at;   /* non-zero: the Nth xfer() from now fails */
static int gone_hook;           /* present() answers 0 */
static uint64_t sent;           /* bytes clocked */

static int t_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    size_t i;

    (void)ctx;
    if (xfer_fail_at != 0 && --xfer_fail_at == 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        rx[i] = sdm_byte(&card, !cs_high, tx[i], hz);
        now_ns += BYTE_NS;
        sent++;
    }
    return 0;
}

static void t_cs(void *ctx, int high)
{
    (void)ctx;
    cs_high = high;
}

static int t_set_hz(void *ctx, uint32_t v)
{
    (void)ctx;
    hz = v;
    return 0;
}

static void t_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    now_ns += (uint64_t)us * 1000u;
}

static uint64_t t_now(void *ctx)
{
    (void)ctx;
    return now_ns / 1000u;
}

static int t_present(void *ctx)
{
    (void)ctx;
    return !gone_hook;
}

static const sdspi_ops_t ops = { NULL, t_xfer, t_cs, t_set_hz, t_delay, t_now, NULL };
static const sdspi_ops_t ops_present = { NULL, t_xfer, t_cs, t_set_hz, t_delay, t_now, t_present };

static sdspi_card_t sc;

/* Fills the backing array with a pattern that differs in every block. */
static void fill(void)
{
    uint32_t i;

    if (mem == NULL) {
        mem = malloc((size_t)BLOCKS * 512u);
    }
    for (i = 0; i < BLOCKS * 512u; i++) {
        mem[i] = (uint8_t)(i * 2654435761u >> 13);
    }
}

static void fresh(int kind)
{
    fill();
    sdm_init(&card, kind, mem, BLOCKS);
    sdm_pins(&card, 1, 1, 1, 0);
    now_ns = 1000000000ull;
    hz = 0;
    cs_high = 1;
    xfer_fail_at = 0;
    gone_hook = 0;
    sent = 0;
}

static int start(int kind)
{
    fresh(kind);
    return sdspi_card_init(&sc, &ops, RUN_HZ);
}

#define NO_VIOLATION() CHECK_STR(card.violations ? card.first : "none", "none")

TEST(crc7_known_commands)
{
    static const uint8_t cmd0[5] = { 0x40, 0, 0, 0, 0 };
    static const uint8_t cmd8[5] = { 0x48, 0, 0, 0x01, 0xaa };
    static const uint8_t cmd17[5] = { 0x51, 0, 0, 0, 0 };

    CHECK_EQ(sdspi_crc7(cmd0, 5), 0x95);
    CHECK_EQ(sdspi_crc7(cmd8, 5), 0x87);
    CHECK_EQ(sdspi_crc7(cmd17, 5), 0x55);
}

TEST(crc16_known_block)
{
    uint8_t b[512];

    memset(b, 0xff, sizeof(b));
    CHECK_EQ(sdspi_crc16(b, sizeof(b)), 0x7fa1);
    CHECK_EQ(sdspi_crc16((const uint8_t *)"123456789", 9), 0x31c3);
}

TEST(init_sdhc)
{
    CHECK_EQ(start(SDM_SDHC), 0);
    CHECK_EQ(sc.ready, 1);
    CHECK_EQ(sc.info.type, SD_TYPE_SDHC);
    CHECK_EQ(sc.info.capacity_blocks, BLOCKS);
    CHECK_EQ(sc.info.ocr & 0xc0000000u, 0xc0000000u);
    CHECK_EQ(sc.info.clock_hz, RUN_HZ);
    CHECK_EQ(sc.info.rca, 0);
    CHECK_EQ(sc.info.mid, 0x03);
    CHECK_STR(sc.info.oid, "SD");
    CHECK_STR(sc.info.pnm, "TAPE1");
    CHECK_EQ(sc.info.prv, 0x42);
    CHECK_EQ(sc.info.psn, 0xdeadbeefu);
    CHECK_EQ(sc.info.mdt_year, 2024);
    CHECK_EQ(sc.info.mdt_month, 6);
    CHECK_EQ(sc.info.cid[0], 0x03534454u);
    CHECK_EQ(sc.info.cid[3] & 0xffu, 0);
    CHECK_EQ(hz, RUN_HZ);
    CHECK_EQ(card.crc_on, 1);
    CHECK_EQ(card.cmds[59], 1);
    CHECK_EQ(card.cmds[16], 0);
    CHECK_EQ(card.cmds[0], 1);
    CHECK_EQ(cs_high, 1);
    NO_VIOLATION();
}

TEST(init_sdsc)
{
    CHECK_EQ(start(SDM_SDSC), 0);
    CHECK_EQ(sc.info.type, SD_TYPE_SDSC);
    CHECK_EQ(sc.info.capacity_blocks, BLOCKS);
    CHECK_EQ(sc.info.ocr & 0x40000000u, 0);
    CHECK_STR(sc.info.pnm, "TAPE2");
    CHECK_EQ(card.cmds[16], 1);
    NO_VIOLATION();
}

TEST(refuses_version_1_and_mmc)
{
    CHECK_EQ(start(SDM_V1), SD_ERR_OLD);
    CHECK_EQ(sc.ready, 0);
    NO_VIOLATION();
    CHECK_EQ(start(SDM_MMC), SD_ERR_OLD);
    NO_VIOLATION();
    CHECK_STR(sd_strerror(SD_ERR_OLD), "old");
}

TEST(no_answer_times_out)
{
    uint64_t t0;

    fresh(SDM_SDHC);
    card.faults = SDM_F_NOANSWER | SDM_F_NEVER_READY;
    card.gone = 1;
    t0 = now_ns;
    CHECK_EQ(sdspi_card_init(&sc, &ops, RUN_HZ), SD_ERR_NOCARD);
    CHECK((now_ns - t0) / 1000u >= SDSPI_CMD0_US);
    CHECK((now_ns - t0) / 1000u <= SDSPI_CMD0_US + 5000u);
    CHECK_EQ(cs_high, 1);
}

TEST(wrong_cmd8_echo)
{
    fresh(SDM_SDHC);
    card.faults = SDM_F_BAD_ECHO;
    CHECK_EQ(sdspi_card_init(&sc, &ops, RUN_HZ), SD_ERR_VOLTAGE);
    CHECK_EQ(cs_high, 1);
}

TEST(acmd41_never_ready_times_out)
{
    uint64_t t0;

    fresh(SDM_SDHC);
    card.faults = SDM_F_NEVER_READY;
    t0 = now_ns;
    CHECK_EQ(sdspi_card_init(&sc, &ops, RUN_HZ), SD_ERR_BUSY);
    CHECK((now_ns - t0) / 1000u >= SDSPI_INIT_US);
    CHECK((now_ns - t0) / 1000u <= SDSPI_INIT_US + 50000u);
    CHECK_EQ(cs_high, 1);
    NO_VIOLATION();
}

TEST(unknown_csd_layout)
{
    fresh(SDM_SDHC);
    card.faults = SDM_F_BAD_CSD;
    CHECK_EQ(sdspi_card_init(&sc, &ops, RUN_HZ), SD_ERR_CSD);
}

TEST(bus_failure_in_init_and_read)
{
    uint8_t b[512];

    fresh(SDM_SDHC);
    xfer_fail_at = 3;   /* the first R1 poll of CMD0 */
    CHECK_EQ(sdspi_card_init(&sc, &ops, RUN_HZ), SD_ERR_BUS);
    CHECK_EQ(start(SDM_SDHC), 0);
    xfer_fail_at = 2;
    CHECK_EQ(sdspi_card_read(&sc, 0, b), BLK_ERR_IO);
    CHECK_EQ(cs_high, 1);
}

TEST(read_before_init_is_nomedia)
{
    sdspi_card_t none;
    uint8_t b[512];

    memset(&none, 0, sizeof(none));
    CHECK_EQ(sdspi_card_read(&none, 0, b), BLK_ERR_NOMEDIA);
    CHECK_EQ(sdspi_card_read(&none, 0, NULL), BLK_ERR_PARAM);
}

static void read_edges(int kind)
{
    uint8_t *b = malloc(512);
    uint8_t raw[515];
    unsigned before;

    CHECK_EQ(start(kind), 0);
    CHECK_EQ(sdspi_card_read(&sc, 0, b), 0);
    CHECK_MEM(b, mem, 512);
    CHECK_EQ(sdspi_card_read(&sc, BLOCKS - 1u, b), 0);
    CHECK_MEM(b, mem + (size_t)(BLOCKS - 1u) * 512u, 512);
    before = card.cmds[17];
    CHECK_EQ(sdspi_card_read(&sc, BLOCKS, b), BLK_ERR_RANGE);
    CHECK_EQ(card.cmds[17], before);    /* refused before it reaches the card */
    /* An unaligned buffer: bytes 1 to 512 of raw, guards untouched. */
    memset(raw, 0x5a, sizeof(raw));
    CHECK_EQ(sdspi_card_read(&sc, 7, raw + 1), 0);
    CHECK_MEM(raw + 1, mem + 7u * 512u, 512);
    CHECK_EQ(raw[0], 0x5a);
    CHECK_EQ(raw[513], 0x5a);
    CHECK_EQ(cs_high, 1);
    NO_VIOLATION();
    free(b);
}

TEST(read_first_last_past_end_unaligned_sdhc) { read_edges(SDM_SDHC); }
TEST(read_first_last_past_end_unaligned_sdsc) { read_edges(SDM_SDSC); }

TEST(read_every_block_matches)
{
    uint8_t *b = malloc((size_t)BLOCKS * 512u);
    uint32_t i;
    int bad = 0;

    CHECK_EQ(start(SDM_SDHC), 0);
    for (i = 0; i < BLOCKS; i++) {
        bad |= sdspi_card_read(&sc, i, b + (size_t)i * 512u) != 0;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(memcmp(b, mem, (size_t)BLOCKS * 512u), 0);
    CHECK_EQ(card.reads, BLOCKS);
    NO_VIOLATION();
    free(b);
}

TEST(read_faults)
{
    uint8_t b[512];
    uint64_t t0;

    CHECK_EQ(start(SDM_SDHC), 0);
    card.faults = SDM_F_BAD_DCRC;
    CHECK_EQ(sdspi_card_read(&sc, 3, b), BLK_ERR_IO);
    card.faults = SDM_F_ERR_TOKEN;
    CHECK_EQ(sdspi_card_read(&sc, 3, b), BLK_ERR_IO);
    card.faults = SDM_F_NO_TOKEN;
    t0 = now_ns;
    CHECK_EQ(sdspi_card_read(&sc, 3, b), BLK_ERR_TIMEOUT);
    CHECK((now_ns - t0) / 1000u >= SDSPI_TOKEN_US);
    CHECK((now_ns - t0) / 1000u <= SDSPI_TOKEN_US + 1000u);
    /* The card's own range check: an error token with the out of range bit. */
    card.faults = 0;
    card.blocks = 100;
    CHECK_EQ(sdspi_card_read(&sc, 200, b), BLK_ERR_RANGE);
    card.blocks = BLOCKS;
    CHECK_EQ(sdspi_card_read(&sc, 3, b), 0);
    CHECK_MEM(b, mem + 3u * 512u, 512);
    CHECK_EQ(cs_high, 1);
    NO_VIOLATION();
}

TEST(present_hook_stops_at_once)
{
    uint8_t b[512];

    fresh(SDM_SDHC);
    CHECK_EQ(sdspi_card_init(&sc, &ops_present, RUN_HZ), 0);
    gone_hook = 1;
    CHECK_EQ(sdspi_card_read(&sc, 0, b), BLK_ERR_NOMEDIA);
    fresh(SDM_SDHC);
    gone_hook = 1;
    CHECK_EQ(sdspi_card_init(&sc, &ops_present, RUN_HZ), SD_ERR_NOCARD);
    CHECK(sent < 64);
}

/* The card vanishes after n bytes of an init: init fails, within its timeouts. */
TEST(removal_at_every_byte_of_init)
{
    uint64_t total;
    uint64_t n;
    unsigned fails = 0;
    unsigned slow = 0;

    CHECK_EQ(start(SDM_SDHC), 0);
    total = card.bytes;
    CHECK(total > 100);
    /* The last byte is the 8 trailing clocks: the card has said all by then. */
    for (n = 0; n + 1 < total; n++) {
        uint64_t t0;
        int rc;

        fresh(SDM_SDHC);
        card.vanish_after = n == 0 ? 1 : n;
        if (n == 0) {
            card.gone = 1;
        }
        t0 = now_ns;
        rc = sdspi_card_init(&sc, &ops, RUN_HZ);
        fails += rc == 0;
        slow += (now_ns - t0) / 1000u > SDSPI_CMD0_US + SDSPI_INIT_US + SDSPI_TOKEN_US;
        if (rc == 0 || cs_high != 1) {
            printf("  vanish after %llu: rc=%d cs=%d\n", (unsigned long long)n, rc, cs_high);
        }
    }
    CHECK_EQ(fails, 0);
    CHECK_EQ(slow, 0);
}

/* The card vanishes after n bytes of a read: an error, and nothing past buf. */
TEST(removal_at_every_byte_of_read)
{
    uint8_t *b = malloc(512);
    uint64_t base;
    uint64_t total;
    uint64_t n;
    unsigned ok = 0;

    CHECK_EQ(start(SDM_SDHC), 0);
    base = card.bytes;
    CHECK_EQ(sdspi_card_read(&sc, 9, b), 0);
    total = card.bytes - base;
    CHECK(total > 512);
    for (n = 0; n + 1 < total; n++) {
        int rc;

        card.gone = 0;
        card.vanish_after = card.bytes + n;
        if (n == 0) {
            card.gone = 1;
        }
        rc = sdspi_card_read(&sc, 9, b);
        if (rc == 0) {
            ok++;
            printf("  vanish after %llu of %llu: read passed\n", (unsigned long long)n,
                   (unsigned long long)total);
        }
        CHECK(rc == BLK_ERR_IO || rc == BLK_ERR_TIMEOUT || rc == 0);
        /* Whatever was lost, the card model still runs for the next try. */
        card.vanish_after = 0;
        card.gone = 0;
        card.nout = card.pos = card.ncmd = 0;
    }
    CHECK_EQ(ok, 0);
    CHECK_EQ(cs_high, 1);
    free(b);
}

/*
 * Stepped init. After every step: no more than SDSPI_STEP_BYTES clocked,
 * CS high with its 8 trailing clocks, and CMD55 never left without its
 * ACMD41. Between two steps the test waits as told, as a caller would.
 */
static unsigned steps;
static uint64_t max_step_bytes;
static unsigned split_pairs;
static unsigned open_steps;

static int run_steps(void)
{
    int rc = sdspi_init_start(&sc, &ops, RUN_HZ);

    steps = 0;
    max_step_bytes = 0;
    split_pairs = 0;
    open_steps = 0;
    while (rc == SD_PENDING) {
        uint64_t b0 = sent;
        unsigned c55 = card.cmds[55];
        unsigned c41 = card.cmds[41];
        uint32_t w = sdspi_init_wait_us(&sc);

        now_ns += (uint64_t)w * 1000u;
        rc = sdspi_init_step(&sc);
        steps++;
        if (sent - b0 > max_step_bytes) {
            max_step_bytes = sent - b0;
        }
        split_pairs += card.cmds[55] - c55 != card.cmds[41] - c41;
        open_steps += !cs_high || card.cs_low || card.clocks_since_rise == 0;
    }
    return rc;
}

static void stepped(int kind, int want, unsigned want_steps)
{
    uint64_t one_shot;

    CHECK_EQ(start(kind), want);
    one_shot = sent;
    fresh(kind);
    CHECK_EQ(run_steps(), want);
    CHECK_EQ(steps, want_steps);
    CHECK_EQ(sent, one_shot);
    CHECK(max_step_bytes <= SDSPI_STEP_BYTES);
    CHECK_EQ(split_pairs, 0);
    CHECK_EQ(open_steps, 0);
    CHECK_EQ(sc.ready, want == 0);
    /* A final result stays. */
    CHECK_EQ(sdspi_init_step(&sc), want);
    CHECK_EQ(sdspi_init_step(&sc), want);
    NO_VIOLATION();
    printf("  kind %d: %u steps, largest %llu bytes\n", kind, steps,
           (unsigned long long)max_step_bytes);
}

/* SDHC: start+CMD0, CMD8, CMD59, 4 ACMD41 (3 busy), CMD58, CMD9, CMD10. */
TEST(stepped_init_sdhc) { stepped(SDM_SDHC, 0, 10); }
/* SDSC adds CMD16. */
TEST(stepped_init_sdsc) { stepped(SDM_SDSC, 0, 11); }
TEST(stepped_init_v1_and_mmc)
{
    stepped(SDM_V1, SD_ERR_OLD, 2);
    stepped(SDM_MMC, SD_ERR_OLD, 2);
}

TEST(stepped_init_matches_the_blocking_one)
{
    sd_info_t a;

    CHECK_EQ(start(SDM_SDHC), 0);
    a = sc.info;
    fresh(SDM_SDHC);
    CHECK_EQ(run_steps(), 0);
    CHECK_MEM(&sc.info, &a, sizeof(a));
    CHECK_EQ(hz, RUN_HZ);
}

/* A card busy for n ACMD41 polls takes n more steps, each one short. */
TEST(stepped_init_busy_card)
{
    static const unsigned busy[] = { 0, 1, 10, 200 };
    unsigned i;

    for (i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        fresh(SDM_SDHC);
        card.acmd41_busy = busy[i];
        CHECK_EQ(run_steps(), 0);
        CHECK_EQ(steps, 7u + busy[i]);
        CHECK(max_step_bytes <= SDSPI_STEP_BYTES);
        CHECK_EQ(split_pairs, 0);
        NO_VIOLATION();
    }
}

/* The pause between two ACMD41 tries is a wait the caller sees, never a delay. */
TEST(stepped_init_retry_is_a_wait)
{
    unsigned i;
    int rc;

    fresh(SDM_SDHC);
    card.acmd41_busy = 5;
    CHECK_EQ(sdspi_init_start(&sc, &ops, RUN_HZ), SD_PENDING);
    CHECK_EQ(sent, 0);
    /* Start clocks and CMD0, CMD8, CMD59: no wait after them. */
    for (i = 0; i < 3; i++) {
        CHECK_EQ(sdspi_init_step(&sc), SD_PENDING);
        CHECK_EQ(sdspi_init_wait_us(&sc), 0);
    }
    CHECK_EQ(sdspi_init_step(&sc), SD_PENDING);
    CHECK_EQ(card.cmds[41], 1);
    rc = sdspi_init_wait_us(&sc) > SDSPI_RETRY_US - 100u;
    CHECK_EQ(rc, 1);
    now_ns += 400000u;
    CHECK(sdspi_init_wait_us(&sc) < SDSPI_RETRY_US - 300u);
    now_ns += 700000u;
    CHECK_EQ(sdspi_init_wait_us(&sc), 0);
}

TEST(stepped_init_never_ready_times_out)
{
    uint64_t t0;

    fresh(SDM_SDHC);
    card.faults = SDM_F_NEVER_READY;
    t0 = now_ns;
    CHECK_EQ(run_steps(), SD_ERR_BUSY);
    CHECK((now_ns - t0) / 1000u >= SDSPI_INIT_US);
    CHECK((now_ns - t0) / 1000u <= SDSPI_INIT_US + 50000u);
    CHECK(max_step_bytes <= SDSPI_STEP_BYTES);
    CHECK_EQ(sc.ready, 0);
}

TEST(stepped_init_bad_args)
{
    CHECK_EQ(sdspi_init_start(NULL, &ops, RUN_HZ), SD_ERR_BUS);
    CHECK_EQ(sdspi_init_start(&sc, NULL, RUN_HZ), SD_ERR_BUS);
    CHECK_EQ(sdspi_init_step(&sc), SD_ERR_BUS);
    CHECK_EQ(sdspi_init_start(&sc, &ops, 0), SD_ERR_BUS);
    CHECK_EQ(sdspi_init_step(NULL), SD_ERR_BUS);
    CHECK_STR(sd_strerror(SD_PENDING), "pending");
}

/* The model's own rule: a command with no start clocks gets SD mode garbage. */
TEST(model_command_without_start_clocks)
{
    static const uint8_t cmd0[6] = { 0x40, 0, 0, 0, 0, 0x95 };
    unsigned i;
    int idle = 0;

    fresh(SDM_SDHC);
    for (i = 0; i < 6; i++) {
        (void)sdm_byte(&card, 1, cmd0[i], 400000);
    }
    for (i = 0; i < 8; i++) {
        idle |= sdm_byte(&card, 1, 0xff, 400000) == 0x01;
    }
    CHECK(card.violations > 0);
    CHECK_STR(card.first, "command before 74 clocks with CS high");
    CHECK_EQ(idle, 0);
}

TEST(model_checks_pin_order)
{
    fresh(SDM_SDHC);
    sdm_pins(&card, 1, 0, 1, 2000000);
    CHECK_STR(card.first, "power off with the bus on");
    fresh(SDM_SDHC);
    sdm_pins(&card, 1, 1, 0, 0);
    sdm_pins(&card, 1, 0, 0, 1000);
    sdm_pins(&card, 1, 1, 0, 50000);
    CHECK_STR(card.first, "power on again less than 100 ms after off");
    fresh(SDM_SDHC);
    sdm_pins(&card, 1, 1, 0, 0);
    sdm_pins(&card, 1, 0, 0, 0);
    sdm_pins(&card, 0, 0, 0, 0);
    sdm_pins(&card, 0, 1, 0, 1000000);
    CHECK_STR(card.first, "power on with no card");
}

TEST(model_checks_rate_and_crc)
{
    static const uint8_t bad[6] = { 0x40, 0, 0, 0, 0, 0x01 };
    unsigned i;

    fresh(SDM_SDHC);
    for (i = 0; i < 10; i++) {
        (void)sdm_byte(&card, 0, 0xff, 25000000);
    }
    CHECK_STR(card.first, "clock over 400 kHz before ACMD41 finished");
    fresh(SDM_SDHC);
    for (i = 0; i < 10; i++) {
        (void)sdm_byte(&card, 0, 0xff, 400000);
    }
    for (i = 0; i < 6; i++) {
        (void)sdm_byte(&card, 1, bad[i], 400000);
    }
    CHECK_STR(card.first, "CMD0 with a bad CRC7");
}
