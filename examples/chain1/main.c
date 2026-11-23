#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/loader.h>
#include <robco/mmu.h>
#include <robco/part.h>
#include <robco/rimg.h>
#include <robco/sd.h>
#include <robco/fs.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>
#include <robco/uart_mini.h>

#define STAGE "chain1"
#define NEXT_ADDR 0x00200000u
#define NEXT_SIZE 0x00200000u

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case 0x13: return "svc";
    case 0x1a: return "hyp";
    default:   return "other";
    }
}

static void halt(void)
{
    k_printf("@RC stage=" STAGE " ev=halt\n");
    uart_flush();
    robco_halt();
}

#if defined(CHAIN1_SD) && CHAIN1_SD
/* The MFBootAgent files and slot, from BOOT_PROTOCOL.md and MEMORY_MAP.md. */
#define PRIMARY "/ROBCO/MFBOOT.SYS"
#define BACKUP  "/ROBCO/MFBOOT.BAK"

static const char *const markers[] = { PRIMARY, BACKUP };
static fs_boot_t vol;
static loader_result_t res;

/* A failed card or mount is an I/O error. The BIOS then enters its shell; this example halts. */
static void fail(const char *code, const char *path, const char *what, int rc)
{
    k_printf("@RC stage=" STAGE " ev=error code=%s path=%s what=%s rc=%d\n", code, path, what, rc);
    k_printf("@RC stage=" STAGE " ev=shell reason=%s\n", code);
    halt();
}

/*
 * Mounts the boot volume with the rule every stage shares, fs_mount_boot(),
 * and loads MFBootAgent. Returns the first LBA of the volume, 0 for a card
 * with no partition table.
 */
static uint32_t load_from_card(void)
{
    const loader_slot_t slot = { NEXT_ADDR, NEXT_SIZE, NULL };
    const rimg_expect_t expect = { .magic = RIMG_MAGIC_BOOT, .abi_major = 2 };
    int rc = sd_init();

    if (rc != 0) {
        fail("io", PRIMARY, "sd", rc);
    }
    rc = fs_mount_boot("sd", sd_blkdev(), markers, 2, &vol);
    if (rc != PART_BOOT_OK) {
        /* "part": no entry to try. "mount": entries, none mounts. */
        fail("io", PRIMARY, rc == PART_BOOT_ERR_MOUNT || rc == PART_BOOT_ERR_GPT ||
             (rc == PART_BOOT_ERR_NOFAT && vol.last_err != 0) ? "mount" : "part",
             vol.last_err);
    }
    k_printf("@RC stage=" STAGE " ev=mount lba=%u blocks=%u\n", (unsigned)vol.start_lba,
             (unsigned)vol.block_count);

    rc = loader_load_fallback(PRIMARY, BACKUP, &slot, &expect, &res);
    if (res.fallback) {
        k_printf("@RC stage=" STAGE " ev=error code=%s path=" PRIMARY "\n",
                 rimg_strerror(res.primary_err));
        k_printf("@RC stage=" STAGE " ev=fallback path=" BACKUP " from=" PRIMARY " to=" BACKUP
                 " reason=%s\n", rimg_strerror(res.primary_err));
    }
    fs_unmount("sd");
    if (rc != RIMG_OK) {
        k_printf("@RC stage=" STAGE " ev=error code=%s path=%s\n", rimg_strerror(rc), res.path);
        k_printf("@RC stage=" STAGE " ev=shell reason=%s\n", rimg_strerror(rc));
        halt();
    }
    return (uint32_t)vol.start_lba;
}
#endif

void robco_main(void)
{
    const robco_board_t *b = board_get();
#if !(defined(CHAIN1_SD) && CHAIN1_SD)
    const rimg_expect_t expect = {
        .magic = RIMG_MAGIC_BOOT,
        .slot_base = NEXT_ADDR,
        .slot_size = NEXT_SIZE,
        .abi_major = 2,
    };
#endif
    const rimg_header_t *h;
    robco_bootinfo_t *bi = (robco_bootinfo_t *)ROBCO_BOOTINFO_ADDR;
    char name[sizeof(h->name) + 1];
    uint32_t fdt_size;
#if defined(CHAIN1_SD) && CHAIN1_SD
    uint32_t part_lba;
#endif
    int rc __attribute__((unused)); /* not used by every variant */

    uart_init(115200);
    k_printf("\n@RC stage=" STAGE " ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));

#if defined(CHAIN1_MMU) && CHAIN1_MMU
    /*
     * Runs the whole stage with the MMU and caches on. The image, boot
     * info and FDT copy are then written through the D-cache, and
     * robco_handoff() must clean them to memory for the next stage.
     */
    rc = mmu_init();
    if (rc != 0) {
        k_printf("@RC stage=" STAGE " ev=error code=mmu rc=%d\n", rc);
        halt();
    }
    mmu_enable();
    k_printf("@RC stage=" STAGE " ev=mmu on=%d\n", mmu_enabled());
#endif

#if defined(CHAIN1_FB) && CHAIN1_FB
    /* Hands a live framebuffer and console to the next stage. */
    rc = fb_init(640, 480, 32);
    if (rc != 0) {
        k_printf("@RC stage=" STAGE " ev=error code=fb rc=%d\n", rc);
        halt();
    }
    k_printf("@RC stage=" STAGE " ev=fb addr=0x%08x\n", (unsigned)fb_get()->addr);
    if (console_init(CONSOLE_SCHEME_AMBER) != 0) {
        k_printf("@RC stage=" STAGE " ev=error code=console\n");
        halt();
    }
    console_puts("chain1\n");
#endif

#if defined(CHAIN1_SD) && CHAIN1_SD
    part_lba = load_from_card();
    h = res.hdr;
    memcpy(name, h->name, sizeof(h->name));
    name[sizeof(h->name)] = '\0';
    k_printf("@RC stage=" STAGE " ev=load path=%s size=%u crc=0x%08x addr=0x%08x name=%s\n",
             res.path, (unsigned)res.file_size, (unsigned)res.crc32, (unsigned)res.entry, name);
#else
    /* The test harness placed the next image in its slot. Nothing reads storage here. */
    rc = rimg_validate((const void *)NEXT_ADDR, NEXT_SIZE, &expect, &h);
    if (rc != RIMG_OK) {
        k_printf("@RC stage=" STAGE " ev=error code=%s\n", rimg_strerror(rc));
        halt();
    }
    memcpy(name, h->name, sizeof(h->name));
    name[sizeof(h->name)] = '\0';
    k_printf("@RC stage=" STAGE " ev=load addr=0x%08x size=%u name=%s crc=0x%08x\n",
             (unsigned)h->load_addr, (unsigned)h->file_size, name, (unsigned)h->crc32);
#endif

#if defined(CHAIN1_TICK) && CHAIN1_TICK
    /*
     * Runs a fast tick, then leaves both ARM timer channels matched with
     * IRQs masked. The handoff must not pass these to the next stage.
     */
    uint32_t hz0 = tick_hz();

    irq_init();
    tick_init(1000);
    irq_cpu_enable();
    while (tick_count() < 20) {
    }
    irq_cpu_disable();
    timer_oneshot_arm(3, timer_now_lo() + 100);
    timer_delay_us(3000);
    k_printf("@RC stage=" STAGE " ev=tick ticks=%u cs1=%d cs3=%d hz0=%u hz=%u\n",
             (unsigned)tick_count(), timer_compare_pending(1) != 0,
             timer_compare_pending(3) != 0, (unsigned)hz0, (unsigned)tick_hz());
#endif

    fdt_size = bootinfo_copy_fw_blob(ROBCO_HANDOFF_FDT_ADDR, ROBCO_HANDOFF_FDT_MAX);
#if defined(CHAIN1_SD) && CHAIN1_SD
    /* bootinfo_build() sets the mini UART and SD caps from the live drivers. */
    uart_mini_init(115200);
#endif
    bootinfo_build(bi, ROBCO_STAGE_BIOS);
#if defined(CHAIN1_BAD_BOOTINFO) && CHAIN1_BAD_BOOTINFO
    /* Breaks the CRC, so the next stage must not trust the block. */
    bi->mem_size ^= 0x1000;
#endif
#if defined(CHAIN1_SD) && CHAIN1_SD
    /* A loader that picks a boot device writes the boot fields after bootinfo_build(). */
    bi->boot_dev = ROBCO_BOOT_DEV_EMMC;
    bi->boot_part_lba = part_lba;
    if (res.fallback) {
        bi->boot_flags |= ROBCO_BOOT_FALLBACK;
    }
    memset(bi->boot_path, 0, sizeof(bi->boot_path));
    memcpy(bi->boot_path, res.path, k_strlen(res.path) < sizeof(bi->boot_path) ?
           k_strlen(res.path) : sizeof(bi->boot_path) - 1);
    bootinfo_seal(bi);
#endif

    k_printf("@RC stage=" STAGE " ev=jump entry=0x%08x\n", (unsigned)h->load_addr);
    uart_flush();
    robco_handoff(h->load_addr, bi, fdt_size != 0 ? ROBCO_HANDOFF_FDT_ADDR : 0);
}
