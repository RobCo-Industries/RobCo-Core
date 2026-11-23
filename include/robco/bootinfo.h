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
#define BOOTINFO_E_NO_FB        (-5)  /* bootinfo_fb_info(): no framebuffer in the block */

#ifndef __ASSEMBLER__
#include <stddef.h>

#include <robco/fb.h>

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

/*
 * Fills `out` from the block's fb_* fields for fb_adopt(). Returns
 * BOOTINFO_OK, or BOOTINFO_E_NO_FB when `bi` is NULL, ROBCO_CAP_FB is
 * clear or fb_addr is 0. Checks nothing else: fb_adopt() does.
 */
int bootinfo_fb_info(const robco_bootinfo_t *bi, fb_info_t *out);

/*
 * Carry-over rule. A field that describes state this stage set up itself
 * comes from this stage. Any other field is carried over from the
 * inherited block, because that state still holds after the handoff.
 * bootinfo_merge() applies the rule per field group:
 *
 * - stage, board fields, ROBCO_CAP_LOCAL_INTC: always this stage.
 * - fw_r0..fw_r2 (BOOTINFO_OWN_FW_REGS): from the firmware, so only a
 *   cold stage sets them.
 * - fdt_addr, fdt_size (BOOTINFO_OWN_FW_BLOB): this stage, if it copied
 *   the blob with bootinfo_copy_fw_blob().
 * - uart_*, ROBCO_CAP_PL011 (BOOTINFO_OWN_UART): this stage, if it
 *   configured the console UART.
 * - ROBCO_CAP_MINIUART (BOOTINFO_OWN_MINIUART): this stage, if it
 *   initialised the mini UART.
 * - ROBCO_CAP_SD (BOOTINFO_OWN_SD): this stage, if it initialised the
 *   SD card.
 * - fb_*, ROBCO_CAP_FB (BOOTINFO_OWN_FB): this stage, if it called
 *   fb_init(). A new framebuffer without a new console clears con_cols,
 *   con_rows, con_cx and con_cy, since the old console drew on the old
 *   framebuffer. con_scheme stays.
 * - con_* (BOOTINFO_OWN_CON): this stage, if its console is active.
 * - boot_dev, boot_part_lba, boot_flags, boot_path, cmdline
 *   (BOOTINFO_OWN_BOOT_DEV): only a cold stage sets them. A loader that
 *   picks a boot device writes them after bootinfo_build() and calls
 *   bootinfo_seal().
 * - Other caps bits: reserved, 0.
 */
#define BOOTINFO_OWN_FW_REGS    (1u << 0)
#define BOOTINFO_OWN_FW_BLOB    (1u << 1)
#define BOOTINFO_OWN_UART       (1u << 2)
#define BOOTINFO_OWN_FB         (1u << 3)
#define BOOTINFO_OWN_CON        (1u << 4)
#define BOOTINFO_OWN_BOOT_DEV   (1u << 5)
#define BOOTINFO_OWN_MINIUART   (1u << 6)
#define BOOTINFO_OWN_SD         (1u << 7)

/*
 * Writes to `out` the merge of `prev`, the inherited block, and `own`,
 * the fields this stage filled. `own_groups` holds the BOOTINFO_OWN_*
 * bits of the groups this stage set up. With `prev` NULL, `out` is a copy
 * of `own`. `out` may equal `prev` or `own`. Does not seal.
 */
void bootinfo_merge(robco_bootinfo_t *out, const robco_bootinfo_t *prev,
                    const robco_bootinfo_t *own, uint32_t own_groups);

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
 * Fills `bi` for the next stage and seals it, following the carry-over
 * rule above. Cold, every field comes from this stage: the entry
 * registers, the last bootinfo_copy_fw_blob(), ROBCO_BOOT_COLD, and the
 * UART, framebuffer and console if this stage set them up, else 0.
 * After the call, bootinfo_get() returns `bi`.
 */
void bootinfo_build(robco_bootinfo_t *bi, uint32_t stage_id);

#endif

#endif
