#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/rimg.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define STAGE "chain2"

static robco_bootinfo_t local;
static robco_bootinfo_t scratch;

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case 0x13: return "svc";
    case 0x1a: return "hyp";
    default:   return "other";
    }
}

/* The boot path as a marker value: "none" when empty, cut at the array end. */
static const char *path_or_none(const char *p, size_t size)
{
    static char buf[sizeof(((robco_bootinfo_t *)0)->boot_path) + 1];

    if (p[0] == '\0') {
        return "none";
    }
    memcpy(buf, p, size < sizeof(buf) - 1 ? size : sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

/* Lines this stage prints. CHAIN2_VIDEO_WAIT prints enough to scroll. */
#if defined(CHAIN2_VIDEO_WAIT) && CHAIN2_VIDEO_WAIT
#define VIDEO_LINES 27
#else
#define VIDEO_LINES 3
#endif

/*
 * The caller pattern from the guide: trust the boot info, then fb_adopt()
 * and console_adopt(). If either fails, fb_init() and console_init().
 * Returns 1 with a console running. The test hooks spoil one value to
 * force the fallback.
 */
static int video_start(const robco_bootinfo_t *bi, int inherited)
{
    fb_info_t info;
    int rc_fb, rc_con = 1;
    unsigned cy;
    const char *path = "adopt";

    if (!inherited || (bi->caps & ROBCO_CAP_FB) == 0) {
        k_printf("@RC stage=" STAGE " ev=video path=none\n");
        return 0;
    }
    rc_fb = bootinfo_fb_info(bi, &info);
#if defined(CHAIN2_BAD_PITCH) && CHAIN2_BAD_PITCH
    info.pitch = info.width * 4 - 4;
#endif
    if (rc_fb == 0) {
        rc_fb = fb_adopt(&info);
    }
    if (rc_fb == 0) {
        cy = bi->con_cy;
#if defined(CHAIN2_BAD_CURSOR) && CHAIN2_BAD_CURSOR
        cy = bi->con_rows;
#endif
        rc_con = console_adopt(bi->con_cols, bi->con_rows, bi->con_cx, cy, bi->con_scheme);
    }
    if (rc_fb != 0 || rc_con != 0) {
        path = "init";
        if (fb_init(640, 480, 32) != 0 ||
            console_init(bi->con_scheme < CONSOLE_SCHEME_COUNT ? bi->con_scheme
                                                               : CONSOLE_SCHEME_GREEN) != 0) {
            k_printf("@RC stage=" STAGE " ev=error code=video\n");
            return 0;
        }
    }
    k_printf("@RC stage=" STAGE " ev=video path=%s fb_rc=%d con_rc=%d fb=0x%08x bi_fb=0x%08x\n",
             path, rc_fb, rc_fb == 0 ? rc_con : 0, (unsigned)(uintptr_t)fb_get()->addr,
             (unsigned)bi->fb_addr);
    return 1;
}

/* With CHAIN2_VIDEO_WAIT, a marker, then one byte from the test: the screen is still. */
static void video_wait(unsigned n)
{
#if defined(CHAIN2_VIDEO_WAIT) && CHAIN2_VIDEO_WAIT
    k_printf("@RC stage=" STAGE " ev=wait n=%u\n", n);
    uart_flush();
    (void)uart_getc();
#else
    (void)n;
#endif
}

static uint32_t rd32(uint32_t addr)
{
    return *(volatile const uint32_t *)addr;
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    const robco_bootinfo_t *bi;
    int inherited = bootinfo_inherited();
    int video;
    unsigned n;

    uart_init(115200);
    k_printf("\n@RC stage=" STAGE " ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));
#if defined(CHAIN2_SELF_CHECK) && CHAIN2_SELF_CHECK
    {
        /* The whole file, no skipped range: the entry registers live in BSS. */
        extern const rimg_header_t robco_rimg;

        k_printf("@RC stage=" STAGE " ev=selfcheck rc=%s size=%u crc=0x%08x\n",
                 rimg_strerror(rimg_self_check()), (unsigned)robco_rimg.file_size,
                 (unsigned)robco_rimg.crc32);
    }
#endif
    {
        uint32_t sctlr;

        /* The previous stage must leave the MMU and caches off: SCTLR bits 0, 2 and 12. */
        __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
        k_printf("@RC stage=" STAGE " ev=handoff trusted=%d sctlr=0x%08x\n", inherited,
                 (unsigned)sctlr);
    }

#if defined(CHAIN2_TICK) && CHAIN2_TICK
    {
        /* No timer match may be left over from the previous stage. */
        int cs1 = timer_compare_pending(1) != 0;
        int cs3 = timer_compare_pending(3) != 0;
        uint64_t start;
        uint32_t ticks;

        irq_init();
        start = timer_now_us();
        tick_init(100);
        irq_enable(IRQ_TIMER3);
        irq_cpu_enable();
        while (!timer_expired(start + 200000)) {
        }
        ticks = tick_count();
        k_printf("@RC stage=" STAGE " ev=tick cs1=%d cs3=%d ticks=%u elapsed_us=%u spurious=%u\n",
                 cs1, cs3, (unsigned)ticks, (unsigned)(timer_now_us() - start),
                 (unsigned)irq_spurious_count());
    }
#endif

    if (!inherited) {
        /* Cold boot: copy the firmware blob and build a fresh block, as a first stage does. */
        bootinfo_copy_fw_blob(ROBCO_HANDOFF_FDT_ADDR, ROBCO_HANDOFF_FDT_MAX);
        bootinfo_build(&local, ROBCO_STAGE_BOOTAGENT);
    }
    bi = bootinfo_get();

    k_printf("@RC stage=" STAGE " ev=bootinfo inherited=%d prev_stage=%u soc=%s mem=0x%08x "
             "fw_r1=0x%08x fdt=0x%08x fdt_size=%u flags=0x%08x caps=0x%08x dev=%u "
             "part_lba=%u path=%s\n",
             inherited, (unsigned)bi->stage, b->soc_name, (unsigned)bi->mem_size,
             (unsigned)bi->fw_r1, (unsigned)bi->fdt_addr, (unsigned)bi->fdt_size,
             (unsigned)bi->boot_flags, (unsigned)bi->caps, (unsigned)bi->boot_dev,
             (unsigned)bi->boot_part_lba, path_or_none(bi->boot_path, sizeof(bi->boot_path)));

    if (bi->fdt_addr == 0 || bi->fdt_size < 8 || rd32(bi->fdt_addr + 4) != ROBCO_ATAG_CORE) {
        k_printf("@RC stage=" STAGE " ev=error code=atags\n");
        uart_flush();
        robco_halt();
    }
    /*
     * The adopted framebuffer and console are the ones the previous stage
     * left running, so the block this stage builds describes them still.
     * It is built before this stage prints, so the cursor is unchanged.
     */
    video = video_start(bi, inherited);
    bootinfo_build(&scratch, ROBCO_STAGE_BOOTAGENT);
    k_printf("@RC stage=" STAGE " ev=rebuild id=%u caps=0x%08x fb=0x%08x fb_w=%u fb_h=%u "
             "cols=%u rows=%u cy=%u scheme=%u fdt=0x%08x\n",
             (unsigned)scratch.stage, (unsigned)scratch.caps, (unsigned)scratch.fb_addr,
             (unsigned)scratch.fb_width, (unsigned)scratch.fb_height,
             (unsigned)scratch.con_cols, (unsigned)scratch.con_rows,
             (unsigned)scratch.con_cy, (unsigned)scratch.con_scheme,
             (unsigned)scratch.fdt_addr);
    if (video) {
        video_wait(1);
        for (n = 1; n <= VIDEO_LINES; n++) {
            if (n == 4) {
                video_wait(2);
            }
            console_puts("chain2 line ");
            console_putc((char)('0' + n / 10));
            console_putc((char)('0' + n % 10));
            console_putc('\n');
        }
#if defined(CHAIN2_VIDEO_WAIT) && CHAIN2_VIDEO_WAIT
        console_dump(uart_putc);
#endif
    }
    k_printf("@RC stage=" STAGE " ev=ready\n");
    uart_flush();
}
