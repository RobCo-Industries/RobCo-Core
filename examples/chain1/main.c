#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/klibc.h>
#include <robco/rimg.h>
#include <robco/uart.h>

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

void robco_main(void)
{
    const robco_board_t *b = board_get();
    const rimg_expect_t expect = {
        .magic = RIMG_MAGIC_BOOT,
        .slot_base = NEXT_ADDR,
        .slot_size = NEXT_SIZE,
        .abi_major = 2,
    };
    const rimg_header_t *h;
    robco_bootinfo_t *bi = (robco_bootinfo_t *)ROBCO_BOOTINFO_ADDR;
    char name[sizeof(h->name) + 1];
    uint32_t fdt_size;
    int rc;

    uart_init(115200);
    k_printf("\n@RC stage=" STAGE " ev=boot midr=0x%08x soc=%s base=0x%08x board_rev=0x%08x "
             "mem=0x%08x entry_mode=%s\n",
             (unsigned)b->midr, b->soc_name, (unsigned)b->periph_base,
             (unsigned)b->board_rev, (unsigned)b->mem_size,
             mode_name(robco_boot_args()->entry_mode));

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

    fdt_size = bootinfo_copy_fw_blob(ROBCO_HANDOFF_FDT_ADDR, ROBCO_HANDOFF_FDT_MAX);
    bootinfo_build(bi, ROBCO_STAGE_BIOS);
#if defined(CHAIN1_BAD_BOOTINFO) && CHAIN1_BAD_BOOTINFO
    /* Breaks the CRC, so the next stage must not trust the block. */
    bi->mem_size ^= 0x1000;
#endif

    k_printf("@RC stage=" STAGE " ev=jump entry=0x%08x\n", (unsigned)h->load_addr);
    uart_flush();
    robco_handoff(h->load_addr, bi, fdt_size != 0 ? ROBCO_HANDOFF_FDT_ADDR : 0);
}
