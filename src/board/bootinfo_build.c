#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/bootinfo.h>
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/klibc.h>
#include <robco/uart.h>

/* Weak, so an image without a framebuffer, console or UART does not link them in. */
#pragma weak fb_get
#pragma weak console_cols
#pragma weak console_rows
#pragma weak console_get_cursor
#pragma weak console_scheme
#pragma weak uart_get_config

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
    const fb_info_t *fb = NULL;
    uint32_t clock, baud;
    uint32_t scheme = 0;
    unsigned cx, cy;

    if (inherited && current != NULL) {
        if (bi != current) {
            memmove(bi, current, sizeof(*bi));
        }
        scheme = bi->con_scheme;
    } else {
        memset(bi, 0, sizeof(*bi));
        bi->fw_r0 = args->r0;
        bi->fw_r1 = args->r1;
        bi->fw_r2 = args->r2;
        bi->fdt_addr = blob_addr;
        bi->fdt_size = blob_size;
        bi->boot_flags = ROBCO_BOOT_COLD;
    }

    bi->stage = stage_id;
    bi->board_rev = b->board_rev;
    bi->soc = (uint32_t)b->soc;
    bi->periph_base = b->periph_base;
    bi->midr = b->midr;
    bi->mem_base = b->mem_base;
    bi->mem_size = b->mem_size;

    bi->caps = 0;
    if (b->soc == ROBCO_SOC_BCM2836 || b->soc == ROBCO_SOC_BCM2837) {
        bi->caps |= ROBCO_CAP_LOCAL_INTC;
    }

    bi->uart_type = ROBCO_UART_NONE;
    bi->uart_clock = 0;
    bi->uart_baud = 0;
    if (uart_get_config != NULL && uart_get_config(&clock, &baud) == 0) {
        bi->uart_type = ROBCO_UART_PL011;
        bi->uart_clock = clock;
        bi->uart_baud = baud;
        bi->caps |= ROBCO_CAP_PL011;
    }

    if (fb_get != NULL) {
        fb = fb_get();
    }
    bi->fb_addr = bi->fb_size = bi->fb_width = bi->fb_height = 0;
    bi->fb_pitch = bi->fb_depth = bi->fb_order = 0;
    if (fb != NULL) {
        bi->fb_addr = (uint32_t)fb->addr;
        bi->fb_size = fb->size;
        bi->fb_width = fb->width;
        bi->fb_height = fb->height;
        bi->fb_pitch = fb->pitch;
        bi->fb_depth = fb->depth;
        bi->fb_order = fb->order;
        bi->caps |= ROBCO_CAP_FB;
    }

    bi->con_cols = bi->con_rows = bi->con_cx = bi->con_cy = 0;
    bi->con_scheme = scheme;
    if (console_cols != NULL && console_rows != NULL && console_get_cursor != NULL &&
        console_cols() != 0) {
        console_get_cursor(&cx, &cy);
        bi->con_cols = (uint16_t)console_cols();
        bi->con_rows = (uint16_t)console_rows();
        bi->con_cx = (uint16_t)cx;
        bi->con_cy = (uint16_t)cy;
        if (console_scheme != NULL) {
            bi->con_scheme = console_scheme();
        }
    }

    bootinfo_seal(bi);
    current = bi;
}
