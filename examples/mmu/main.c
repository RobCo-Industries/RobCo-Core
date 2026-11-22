#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/cache.h>
#include <robco/fb.h>
#include <robco/klibc.h>
#include <robco/mbox.h>
#include <robco/mmio.h>
#include <robco/mmu.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define STAGE       "mmu"
#define WIDTH       640
#define HEIGHT      480
#define FILLS       8
#define COPY_SIZE   (4u << 20)
#define COPY_SRC    0x01000000u     /* RAM above the image slot, unused here */
#define COPY_DST    0x01400000u
#define TRAFFIC     0x01800000u     /* other memory traffic between mailbox calls */
#define TRAFFIC_SZ  (256u << 10)
#define RO_SECTION  0x01c00000u     /* remapped read-only for the fault check */
#define UNMAPPED    0x60000000u     /* outside RAM and peripherals on every board */
#define LOOPS       100
#define SQ          16

/*
 * A caller-owned property buffer that starts 16 bytes into a cache line,
 * with live data on both sides. mbox_call() must not lose the neighbours.
 */
static struct {
    uint32_t head[4];
    uint32_t msg[8];
    uint32_t tail[4];
} shared __attribute__((aligned(CACHE_LINE_MAX)));

/*
 * A 40-byte message 32 bytes into a 64-byte line: misaligned at both ends
 * for 32 and 64-byte lines. The guards share its lines and change between
 * calls.
 */
#define GUARD_MSG_WORDS 10
static struct {
    uint32_t lo[8];
    uint32_t msg[GUARD_MSG_WORDS];
    uint32_t hi[6];
} guarded __attribute__((aligned(CACHE_LINE_MAX)));

static void fail(const char *what, int rc)
{
    k_printf("@RC stage=" STAGE " ev=error what=%s rc=%d\n", what, rc);
    uart_flush();
    robco_halt();
}

static void bench(const char *caches)
{
    uint64_t t0;
    uint32_t fill_us, copy_us;
    unsigned i;

    t0 = timer_now_us();
    for (i = 0; i < FILLS; i++) {
        fb_clear(fb_rgb((uint8_t)(i * 32), 0, (uint8_t)(255 - i * 32)));
    }
    fill_us = (uint32_t)(timer_now_us() - t0);

    t0 = timer_now_us();
    memcpy((void *)COPY_DST, (const void *)COPY_SRC, COPY_SIZE);
    copy_us = (uint32_t)(timer_now_us() - t0);

    k_printf("@RC stage=" STAGE " ev=bench caches=%s fills=%u fill_us=%u copy_us=%u\n",
             caches, FILLS, (unsigned)fill_us, (unsigned)copy_us);
}

/* Board revision and UART clock through the library, plus a raw call on `shared`. */
static int mbox_values(uint32_t *rev, uint32_t *clk, uint32_t *raw, uint32_t seed)
{
    unsigned i;
    int bad = 0;

    if (mbox_get_board_rev(rev) != 0 || mbox_get_clock_rate(MBOX_CLOCK_UART, clk) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        shared.head[i] = seed + i;
        shared.tail[i] = ~(seed + i);
    }
    shared.msg[0] = sizeof(shared.msg);
    shared.msg[1] = 0;
    shared.msg[2] = MBOX_TAG_BOARD_REV;
    shared.msg[3] = 8;
    shared.msg[4] = 0;
    shared.msg[5] = 0;
    shared.msg[6] = 0;
    shared.msg[7] = 0;
    if (mbox_call(shared.msg) != 0) {
        return -1;
    }
    *raw = shared.msg[5];
    for (i = 0; i < 4; i++) {
        bad |= shared.head[i] != seed + i;
        bad |= shared.tail[i] != ~(seed + i);
    }
    return bad ? -2 : 0;
}

/* Returns 0, or bit 0 for a lost guard and bit 1 for a wrong reply. */
static int mbox_guarded(uint32_t rev, uint32_t seed)
{
    unsigned i;
    int bad = 0;

    for (i = 0; i < 8; i++) {
        guarded.lo[i] = seed ^ i;
    }
    for (i = 0; i < 6; i++) {
        guarded.hi[i] = ~(seed ^ i);
    }
    k_memset(guarded.msg, 0, sizeof(guarded.msg));
    guarded.msg[0] = sizeof(guarded.msg);
    guarded.msg[2] = MBOX_TAG_BOARD_REV;
    guarded.msg[3] = 8;
    if (mbox_call(guarded.msg) != 0 || guarded.msg[5] != rev) {
        bad |= 2;
    }
    for (i = 0; i < 8; i++) {
        bad |= guarded.lo[i] != (seed ^ i);
    }
    for (i = 0; i < 6; i++) {
        bad |= guarded.hi[i] != ~(seed ^ i);
    }
    return bad;
}

static void draw_pattern(void)
{
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
    };
    unsigned i;

    for (i = 0; i < 8; i++) {
        fb_fill_rect(i * (WIDTH / 8), 0, WIDTH / 8, HEIGHT,
                     fb_rgb(bars[i][0], bars[i][1], bars[i][2]));
    }
    fb_fill_rect(0, 0, SQ, SQ, fb_rgb(255, 0, 0));
    fb_fill_rect(WIDTH - SQ, 0, SQ, SQ, fb_rgb(0, 255, 0));
    fb_fill_rect(0, HEIGHT - SQ, SQ, SQ, fb_rgb(0, 0, 255));
}

static uint32_t sctlr_read(void)
{
    uint32_t v;

    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    uint32_t rev0, clk0, raw0, rev, clk, raw, value;
    unsigned i, mismatches = 0, bad_guard, bad_reply;
    uint64_t deadline;
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=" STAGE " ev=boot soc=%s\n", b->soc_name);

    rc = fb_init(WIDTH, HEIGHT, 32);
    if (rc != 0) {
        fail("fb_init", rc);
    }
    if (mbox_values(&rev0, &clk0, &raw0, 0x1000) != 0) {
        fail("mbox_before", 0);
    }
    k_printf("@RC stage=" STAGE " ev=mbox phase=before rev=0x%08x clk=%u raw=0x%08x\n",
             (unsigned)rev0, (unsigned)clk0, (unsigned)raw0);

    bench("off");

    rc = mmu_init();
    if (rc != 0) {
        fail("mmu_init", rc);
    }
    mmu_enable();
    k_printf("@RC stage=" STAGE " ev=mmu sctlr=0x%08x smp=%s midr=0x%08x\n",
             (unsigned)sctlr_read(), mmu_smp() < 0 ? "na" : mmu_smp() ? "1" : "0",
             (unsigned)b->midr);
    if (!mmu_enabled()) {
        fail("mmu_enable", 0);
    }

    bench("on");

    /* Faults: the probe path recovers from a translation fault. */
    rc = mmu_map(RO_SECTION, MMU_SECTION_SIZE, MMU_ATTR_RO_CODE);
    if (rc != 0) {
        fail("mmu_map", rc);
    }
    k_printf("@RC stage=" STAGE " ev=probe unmapped=%s device=%s ro_read=%s\n",
             mmio_probe(UNMAPPED, &value) ? "abort" : "ok",
             mmio_probe(b->periph_base + 0x200000, &value) ? "abort" : "ok",
             mmio_probe(RO_SECTION, &value) ? "abort" : "ok");
#if defined(MMU_FAULT_RO) && MMU_FAULT_RO
    /* Permission fault: data abort, DFSR status 0x0d (section). */
    uart_flush();
    *(volatile uint32_t *)RO_SECTION = 1;
#endif
#if defined(MMU_FAULT_UNMAPPED) && MMU_FAULT_UNMAPPED
    /* Translation fault: data abort, DFSR status 0x05 (section). */
    uart_flush();
    *(volatile uint32_t *)UNMAPPED = 1;
#endif

    /* Coherence: mailbox answers stay right with other cached traffic in between. */
    for (i = 0; i < LOOPS; i++) {
        k_memset((void *)TRAFFIC, (int)i, TRAFFIC_SZ);
        memcpy((void *)COPY_DST, (const void *)TRAFFIC, TRAFFIC_SZ);
        rc = mbox_values(&rev, &clk, &raw, 0x2000 + i * 16);
        if (rc != 0 || rev != rev0 || clk != clk0 || raw != raw0) {
            mismatches++;
        }
    }
    k_printf("@RC stage=" STAGE " ev=mbox phase=on loops=%u mismatches=%u rev=0x%08x clk=%u "
             "raw=0x%08x\n", LOOPS, mismatches, (unsigned)rev, (unsigned)clk, (unsigned)raw);

    /* Misaligned message, guards rewritten before every call. */
    for (i = 0, bad_guard = 0, bad_reply = 0; i < LOOPS; i++) {
        rc = mbox_guarded(rev0, 0x5a000000u + i);
        bad_guard += (rc & 1) != 0;
        bad_reply += (rc & 2) != 0;
    }
    k_printf("@RC stage=" STAGE " ev=guard loops=%u bad_guard=%u bad_reply=%u\n",
             LOOPS, bad_guard, bad_reply);

    /* No flush between drawing and the GPU reading the frame. */
    draw_pattern();
    k_printf("@RC stage=" STAGE " ev=pattern\n");
    /* Wait for any key (the test takes a screenshot first), or 10 s. */
    deadline = timer_now_us() + 10000000;
    while (uart_poll() < 0 && !timer_expired(deadline)) {
    }

    mmu_disable();
    k_printf("@RC stage=" STAGE " ev=mmu_off sctlr=0x%08x\n", (unsigned)sctlr_read());
    if (mbox_values(&rev, &clk, &raw, 0x3000) != 0) {
        fail("mbox_after", 0);
    }
    k_printf("@RC stage=" STAGE " ev=mbox phase=after rev=0x%08x clk=%u raw=0x%08x\n",
             (unsigned)rev, (unsigned)clk, (unsigned)raw);
    k_printf("@RC stage=" STAGE " ev=done\n");
    uart_flush();
    for (;;) {
    }
}
