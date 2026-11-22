#ifndef ROBCO_BOOTINFO_H
#define ROBCO_BOOTINFO_H

/*
 * Boot info block passed between terminal stages. See docs/spec/BOOT_PROTOCOL.md.
 *
 * Trust rule. Unless robco_config.h sets ROBCO_FIRST_STAGE to 1,
 * robco_start() checks the block before robco_main(): r3 at entry equals
 * ROBCO_BOOTINFO_ADDR, the block at r3 passes bootinfo_validate(), and its
 * midr equals this CPU's MIDR. Then bootinfo_get() returns that block and
 * bootinfo_inherited() returns 1.
 *
 * Otherwise the image cold-boots: bootinfo_inherited() returns 0 and
 * bootinfo_get() returns NULL until the image calls bootinfo_build().
 * After that call, bootinfo_get() returns the block it built. A first
 * stage always cold-boots and ignores r3.
 */

#include <stdint.h>

#ifndef ROBCO_FIRST_STAGE
#define ROBCO_FIRST_STAGE 0
#endif

#define ROBCO_BOOTINFO_ADDR     0x00100000u
#define ROBCO_BOOTINFO_MAGIC    0x524F4243u  /* "CBOR" in memory, reads "ROBC" as a word */
#define ROBCO_BOOTINFO_VERSION  2u
#define ROBCO_BOOTINFO_SIZE     320u
#define ROBCO_HANDOFF_FDT_ADDR  0x00101000u
#define ROBCO_HANDOFF_FDT_MAX   0x0001F000u

/* Stage ids. */
#define ROBCO_STAGE_NONE        0u
#define ROBCO_STAGE_BIOS        1u
#define ROBCO_STAGE_BOOTAGENT   2u
#define ROBCO_STAGE_UOS         3u
#define ROBCO_STAGE_MAINT       4u
#define ROBCO_STAGE_DIAG        5u
#define ROBCO_STAGE_PIPOS       6u
#define ROBCO_STAGE_DEITRIX     7u

/* caps bits. */
#define ROBCO_CAP_FB            (1u << 0)
#define ROBCO_CAP_PL011         (1u << 1)
#define ROBCO_CAP_MINIUART      (1u << 2)
#define ROBCO_CAP_SD            (1u << 3)
#define ROBCO_CAP_LOCAL_INTC    (1u << 4)

/* boot_flags bits. */
#define ROBCO_BOOT_FALLBACK     (1u << 0)
#define ROBCO_BOOT_MAINT        (1u << 1)
#define ROBCO_BOOT_COLD         (1u << 2)

/* uart_type values. */
#define ROBCO_UART_NONE         0u
#define ROBCO_UART_PL011        1u
#define ROBCO_UART_MINI         2u

/* boot_dev values. */
#define ROBCO_BOOT_DEV_UNKNOWN  0u
#define ROBCO_BOOT_DEV_EMMC     1u
#define ROBCO_BOOT_DEV_SDHOST   2u

/* Firmware blob tags. */
#define ROBCO_FDT_MAGIC         0xD00DFEEDu
#define ROBCO_ATAG_CORE         0x54410001u
#define ROBCO_ATAG_NONE         0x00000000u

/* bootinfo_validate() results. */
#define BOOTINFO_OK             0
#define BOOTINFO_E_MAGIC        (-1)
#define BOOTINFO_E_VERSION      (-2)
#define BOOTINFO_E_SIZE         (-3)
#define BOOTINFO_E_CRC          (-4)

#ifndef __ASSEMBLER__
#include <stddef.h>

typedef struct __attribute__((packed)) robco_bootinfo {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t crc32;
    uint32_t stage;
    uint32_t board_rev;
    uint32_t soc;
    uint32_t periph_base;
    uint32_t midr;
    uint32_t mem_base;
    uint32_t mem_size;
    uint32_t fw_r0;
    uint32_t fw_r1;
    uint32_t fw_r2;
    uint32_t fdt_addr;
    uint32_t fdt_size;
    uint32_t caps;
    uint32_t uart_type;
    uint32_t uart_clock;
    uint32_t uart_baud;
    uint32_t fb_addr;
    uint32_t fb_size;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_depth;
    uint32_t fb_order;
    uint16_t con_cols;
    uint16_t con_rows;
    uint16_t con_cx;
    uint16_t con_cy;
    uint32_t con_scheme;
    uint32_t boot_dev;
    uint32_t boot_part_lba;
    uint32_t boot_flags;
    char     boot_path[64];
    char     cmdline[128];
} robco_bootinfo_t;

_Static_assert(sizeof(robco_bootinfo_t) == ROBCO_BOOTINFO_SIZE, "robco_bootinfo_t must be 320 bytes");
_Static_assert(offsetof(robco_bootinfo_t, crc32) == 0x008, "crc32 offset");
_Static_assert(offsetof(robco_bootinfo_t, fw_r0) == 0x028, "fw_r0 offset");
_Static_assert(offsetof(robco_bootinfo_t, fdt_addr) == 0x034, "fdt_addr offset");
_Static_assert(offsetof(robco_bootinfo_t, uart_type) == 0x040, "uart_type offset");
_Static_assert(offsetof(robco_bootinfo_t, fb_addr) == 0x04C, "fb_addr offset");
_Static_assert(offsetof(robco_bootinfo_t, con_cols) == 0x068, "con_cols offset");
_Static_assert(offsetof(robco_bootinfo_t, con_scheme) == 0x070, "con_scheme offset");
_Static_assert(offsetof(robco_bootinfo_t, boot_flags) == 0x07C, "boot_flags offset");
_Static_assert(offsetof(robco_bootinfo_t, boot_path) == 0x080, "boot_path offset");
_Static_assert(offsetof(robco_bootinfo_t, cmdline) == 0x0C0, "cmdline offset");

/* Pure functions, src/lib/bootinfo.c. */

/* Sets magic, version and size, then computes crc32. */
void bootinfo_seal(robco_bootinfo_t *bi);

/* Checks magic, version, size and CRC in that order. BOOTINFO_OK or BOOTINFO_E_*. */
int bootinfo_validate(const robco_bootinfo_t *bi);

/*
 * Size of the firmware blob at `blob`: an FDT by its big-endian totalsize,
 * or an ATAGS list that starts with ATAG_CORE, up to and including
 * ATAG_NONE. Reads at most `max` bytes. Returns 0 if there is no blob or
 * it is larger than `max`.
 */
uint32_t bootinfo_fw_blob_size(const void *blob, uint32_t max);

/* Hardware side, src/board/bootinfo_build.c. */

/* The trusted or built block, or NULL. See the trust rule above. */
const robco_bootinfo_t *bootinfo_get(void);

/* 1 if the block from the previous stage passed the trust rule. */
int bootinfo_inherited(void);

/*
 * Applies the trust rule. robco_start() calls it before robco_main()
 * unless ROBCO_FIRST_STAGE is 1.
 */
void bootinfo_receive(void);

/*
 * Copies the FDT or ATAGS found at the saved firmware r2 to `dest`.
 * Returns the size, or 0 if there is none or it is larger than `max`.
 * bootinfo_build() records the copy in fdt_addr and fdt_size.
 */
uint32_t bootinfo_copy_fw_blob(uint32_t dest, uint32_t max);

/*
 * Fills `bi` for the next stage and seals it. With an inherited block it
 * keeps the firmware registers, the DTB copy and the boot_* fields from
 * that block. Cold, it takes them from the entry registers and the last
 * bootinfo_copy_fw_blob(), and sets ROBCO_BOOT_COLD. It then refreshes
 * the board, UART, framebuffer and console fields. With an active console
 * con_scheme is console_scheme(). Otherwise it is the inherited value, or
 * 0 when cold.
 * After the call, bootinfo_get() returns `bi`.
 */
void bootinfo_build(robco_bootinfo_t *bi, uint32_t stage_id);

#endif

#endif
