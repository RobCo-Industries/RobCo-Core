#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/klibc.h>
#include <robco/sd.h>
#include <robco/uart.h>
#include <robco/uart_mini.h>

/* Weak, so an image without a framebuffer, console, UART or SD card does not link them in. */
#pragma weak fb_get
#pragma weak console_cols
#pragma weak console_rows
#pragma weak console_get_cursor
#pragma weak console_scheme
#pragma weak uart_get_config
#pragma weak uart_mini_ready
#pragma weak sd_blkdev

static const robco_bootinfo_t *current;
static int inherited;
static uint32_t blob_addr;
static uint32_t blob_size;

const robco_bootinfo_t *bootinfo_get(void)
{
    return current;
}

int bootinfo_inherited(void)
{
    return inherited;
}

void bootinfo_receive(void)
{
    const robco_bootinfo_t *bi = (const robco_bootinfo_t *)ROBCO_BOOTINFO_ADDR;

    current = NULL;
    inherited = 0;
    if (robco_boot_args()->r3 != ROBCO_BOOTINFO_ADDR) {
        return;
    }
    if (bootinfo_validate(bi) != BOOTINFO_OK || bi->midr != board_get()->midr) {
        return;
    }
    current = bi;
    inherited = 1;
}

uint32_t bootinfo_copy_fw_blob(uint32_t dest, uint32_t max)
{
    uint32_t src = robco_boot_args()->r2;
    uint32_t size = 0;

    if (src != 0) {
        size = bootinfo_fw_blob_size((const void *)src, max);
    }
    if (size != 0) {
        memmove((void *)dest, (const void *)src, size);
    }
    blob_addr = size != 0 ? dest : 0;
    blob_size = size;
    return size;
}

void bootinfo_build(robco_bootinfo_t *bi, uint32_t stage_id)
{
    const robco_board_t *b = board_get();
    const robco_boot_args_t *args = robco_boot_args();
    const robco_bootinfo_t *prev = (inherited && current != NULL) ? current : NULL;
    const fb_info_t *fb = NULL;
    robco_bootinfo_t own;
    uint32_t groups = 0;
    uint32_t clock, baud;
    unsigned cx, cy;

    memset(&own, 0, sizeof(own));
    own.stage = stage_id;
    own.board_rev = b->board_rev;
    own.soc = (uint32_t)b->soc;
    own.periph_base = b->periph_base;
    own.midr = b->midr;
    own.mem_base = b->mem_base;
    own.mem_size = b->mem_size;
    if (b->soc == ROBCO_SOC_BCM2836 || b->soc == ROBCO_SOC_BCM2837) {
        own.caps |= ROBCO_CAP_LOCAL_INTC;
    }

    own.fw_r0 = args->r0;
    own.fw_r1 = args->r1;
    own.fw_r2 = args->r2;
    own.boot_flags = ROBCO_BOOT_COLD;

    if (blob_size != 0) {
        own.fdt_addr = blob_addr;
        own.fdt_size = blob_size;
        groups |= BOOTINFO_OWN_FW_BLOB;
    }

    if (uart_get_config != NULL && uart_get_config(&clock, &baud) == 0) {
        own.uart_type = ROBCO_UART_PL011;
        own.uart_clock = clock;
        own.uart_baud = baud;
        own.caps |= ROBCO_CAP_PL011;
        groups |= BOOTINFO_OWN_UART;
    }

    if (uart_mini_ready != NULL && uart_mini_ready()) {
        own.caps |= ROBCO_CAP_MINIUART;
        groups |= BOOTINFO_OWN_MINIUART;
    }

    if (sd_blkdev != NULL && sd_blkdev() != NULL) {
        own.caps |= ROBCO_CAP_SD;
        groups |= BOOTINFO_OWN_SD;
    }

    if (fb_get != NULL) {
        fb = fb_get();
    }
    if (fb != NULL) {
        own.fb_addr = (uint32_t)fb->addr;
        own.fb_size = fb->size;
        own.fb_width = fb->width;
        own.fb_height = fb->height;
        own.fb_pitch = fb->pitch;
        own.fb_depth = fb->depth;
        own.fb_order = fb->order;
        own.caps |= ROBCO_CAP_FB;
        groups |= BOOTINFO_OWN_FB;
    }

    if (console_cols != NULL && console_rows != NULL && console_get_cursor != NULL &&
        console_cols() != 0) {
        console_get_cursor(&cx, &cy);
        own.con_cols = (uint16_t)console_cols();
        own.con_rows = (uint16_t)console_rows();
        own.con_cx = (uint16_t)cx;
        own.con_cy = (uint16_t)cy;
        if (console_scheme != NULL) {
            own.con_scheme = console_scheme();
        }
        groups |= BOOTINFO_OWN_CON;
    }

    bootinfo_merge(bi, prev, &own, groups);
    bootinfo_seal(bi);
    current = bi;
}
