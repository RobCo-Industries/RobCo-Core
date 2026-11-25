/*
 * src/drv/wdog.c against a fake PM block: every write is logged with its
 * password byte, the counter can be set by hand to fake a countdown.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>

#define FAKE_BASE   0x20000000u
#define PM          (FAKE_BASE + 0x100000u)
#define RSTC        (PM + 0x1cu)
#define RSTS        (PM + 0x20u)
#define WDOG        (PM + 0x24u)

static uint32_t reg_rstc;
static uint32_t reg_rsts;
static uint32_t reg_wdog;
static uint32_t log_addr[16];
static uint32_t log_val[16];
static unsigned log_n;
static int bad_access;

static uint32_t *fake_reg(uint32_t addr)
{
    switch (addr) {
    case RSTC: return &reg_rstc;
    case RSTS: return &reg_rsts;
    case WDOG: return &reg_wdog;
    default: bad_access++; return NULL;
    }
}

static uint32_t fake_read(uint32_t addr)
{
    uint32_t *r = fake_reg(addr);

    return r ? *r : 0;
}

/* Like the hardware: a write without the password is dropped, the byte never stored. */
static void fake_write(uint32_t addr, uint32_t v)
{
    uint32_t *r = fake_reg(addr);

    if (log_n < 16) {
        log_addr[log_n] = addr;
        log_val[log_n] = v;
    }
    log_n++;
    if (r && (v & 0xff000000u) == 0x5a000000u) {
        *r = v & 0x00ffffffu;
    }
}

/* mmio.h holds ARM barriers: keep it out and route the driver here. */
#define ROBCO_MMIO_H
#define mmio_read(a) fake_read(a)
#define mmio_write(a, v) fake_write((a), (v))
#define mmio_dsb() ((void)0)

#include "../../src/drv/wdog.c"

#include "rc_test.h"

static robco_board_t board = { .periph_base = FAKE_BASE };

const robco_board_t *board_get(void)
{
    return &board;
}

static void fresh(void)
{
    wdog_stop();
    board.periph_base = FAKE_BASE;
    reg_rstc = 0x102;           /* reset values QEMU and Linux report */
    reg_rsts = 0x1000;
    reg_wdog = 0;
    log_n = 0;
    bad_access = 0;
}

static void all_writes_have_password(void)
{
    unsigned i;

    for (i = 0; i < log_n && i < 16; i++) {
        CHECK_EQ(log_val[i] & 0xff000000u, 0x5a000000u);
    }
}

TEST(start_writes_wdog_then_rstc)
{
    fresh();
    CHECK_EQ(wdog_start(1000), 0);
    CHECK_EQ(log_n, 2);
    CHECK_EQ(log_addr[0], WDOG);
    CHECK_EQ(log_val[0], 0x5a000000u | 65536u);
    CHECK_EQ(log_addr[1], RSTC);
    /* Keeps the other RSTC bits, sets WRCFG to full reset. */
    CHECK_EQ(log_val[1], 0x5a000000u | 0x102u | 0x20u);
    all_writes_have_password();
    CHECK(wdog_running());
    CHECK_EQ(wdog_timeout_ms(), 1000);
    CHECK_EQ(reg_rsts, 0x1000);  /* the partition bits stay untouched */
    CHECK_EQ(bad_access, 0);
}

TEST(rounding_low_end)
{
    fresh();
    CHECK_EQ(wdog_start(0), WDOG_ERR_ARG);
    CHECK_EQ(log_n, 0);
    CHECK(!wdog_running());
    CHECK_EQ(wdog_start(WDOG_TIMEOUT_MIN_MS), 0);
    CHECK_EQ(reg_wdog, 66);     /* 65.536 ticks rounds up */
    CHECK_EQ(wdog_start(2), 0);
    CHECK_EQ(reg_wdog, 132);    /* 131.072 */
}

TEST(rounding_high_end)
{
    fresh();
    CHECK_EQ(wdog_start(WDOG_TIMEOUT_MAX_MS), 0);
    CHECK_EQ(reg_wdog, 1048511);    /* 1048510.464 rounds up */
    CHECK(reg_wdog <= 0xfffffu);
    log_n = 0;
    CHECK_EQ(wdog_start(WDOG_TIMEOUT_MAX_MS + 1), WDOG_ERR_ARG);
    CHECK_EQ(wdog_start(UINT32_MAX), WDOG_ERR_ARG);
    CHECK_EQ(log_n, 0);
    /* A refused start leaves the running one alone. */
    CHECK_EQ(wdog_timeout_ms(), WDOG_TIMEOUT_MAX_MS);
    CHECK(wdog_running());
}

TEST(exact_ticks)
{
    fresh();
    CHECK_EQ(wdog_start(500), 0);
    CHECK_EQ(reg_wdog, 32768);
    CHECK_EQ(wdog_start(15000), 0);
    CHECK_EQ(reg_wdog, 983040);
}

TEST(kick_before_start)
{
    fresh();
    CHECK_EQ(wdog_kick(), WDOG_ERR_STOPPED);
    CHECK_EQ(log_n, 0);
    CHECK(!wdog_running());
    CHECK_EQ(wdog_remaining_ms(), 0);
    CHECK_EQ(wdog_timeout_ms(), 0);
}

TEST(kick_reloads)
{
    fresh();
    CHECK_EQ(wdog_start(2000), 0);
    reg_wdog = 100;             /* the counter ran down */
    log_n = 0;
    CHECK_EQ(wdog_kick(), 0);
    CHECK_EQ(log_n, 2);
    CHECK_EQ(log_addr[0], WDOG);
    CHECK_EQ(log_val[0], 0x5a000000u | 131072u);
    CHECK_EQ(log_addr[1], RSTC);
    CHECK_EQ(log_val[1] & 0x30u, 0x20u);
    all_writes_have_password();
    CHECK_EQ(reg_wdog, 131072);
}

TEST(remaining)
{
    fresh();
    CHECK_EQ(wdog_start(3000), 0);
    CHECK_EQ(wdog_remaining_ms(), 3000);
    reg_wdog = 65535;           /* just under 1 s rounds down */
    CHECK_EQ(wdog_remaining_ms(), 999);
    reg_wdog = 0;
    CHECK_EQ(wdog_remaining_ms(), 0);
    reg_wdog = 0xfff00000u | 65536u;    /* bits above the counter are ignored */
    CHECK_EQ(wdog_remaining_ms(), 1000);
}

TEST(stop)
{
    fresh();
    CHECK_EQ(wdog_start(1000), 0);
    log_n = 0;
    wdog_stop();
    CHECK_EQ(log_n, 1);
    CHECK_EQ(log_addr[0], RSTC);
    CHECK_EQ(log_val[0], 0x5a000102u);
    CHECK(!wdog_running());
    CHECK_EQ(wdog_timeout_ms(), 0);
    CHECK_EQ(wdog_remaining_ms(), 0);
    CHECK_EQ(wdog_kick(), WDOG_ERR_STOPPED);
    CHECK_EQ(log_n, 1);
    /* Twice is harmless. */
    wdog_stop();
    CHECK(!wdog_running());
}

TEST(start_twice_replaces)
{
    fresh();
    CHECK_EQ(wdog_start(1000), 0);
    CHECK_EQ(wdog_start(250), 0);
    CHECK_EQ(reg_wdog, 16384);
    CHECK_EQ(wdog_timeout_ms(), 250);
    CHECK_EQ(reg_rstc & 0x30u, 0x20u);
    CHECK_EQ(wdog_kick(), 0);
    CHECK_EQ(reg_wdog, 16384);
}

TEST(unknown_board)
{
    fresh();
    board.periph_base = 0;
    CHECK_EQ(wdog_start(1000), WDOG_ERR_INIT);
    CHECK(!wdog_running());
    CHECK_EQ(wdog_kick(), WDOG_ERR_STOPPED);
    CHECK_EQ(wdog_remaining_ms(), 0);
    wdog_stop();
    CHECK_EQ(log_n, 0);
}

/* An RSTC configured for full reset by someone else still reads as running. */
TEST(running_reads_hardware)
{
    fresh();
    reg_rstc = 0x122;
    CHECK(wdog_running());
    CHECK_EQ(wdog_timeout_ms(), 0);
    CHECK_EQ(wdog_kick(), WDOG_ERR_STOPPED);
}
