# Boot protocol

## Chains

| Chain | Order |
|---|---|
| Terminal | Firmware → RETROS-BIOS → MFBootAgent → UOS, MAINT or DIAG |
| Pip-Boy | Firmware → Pip-OS → Deitrix ROM → holotapes |

The chains are separate. MFBootAgent never loads Pip-OS. Every stage also boots alone, straight from the firmware or QEMU, with no earlier stage.

## Finding, loading and entering the next stage

| Stage | Loads | Path | Slot base |
|---|---|---|---|
| RETROS-BIOS | MFBootAgent | `/ROBCO/MFBOOT.SYS`, then `/ROBCO/MFBOOT.BAK` | `0x00200000` |
| MFBootAgent | Entry chosen in the menu | From `/ROBCO/BOOT.CFG`. Default `/ROBCO/UOS.SYS`. | `0x00400000` |
| Pip-OS | Deitrix | `/PIPBOY/ROM1.ROM` | `0x00400000` |
| Pip-OS | Holotape, when Deitrix asks | `/PIPBOY/HOLO/<NAME>.HTP` | `0x00800000` |

Each loader:

1. Mounts the FAT partition on the SD card through FatFs.
2. Opens the file and validates it as described in [IMAGE_FORMAT.md](./IMAGE_FORMAT.md). Emits `ev=load` before the read and `ev=error` on failure.
3. Updates the boot info, as described below.
4. Prepares the CPU for the jump.
5. Jumps to `load_addr` with the registers in the next table. Emits `ev=jump` right before.

CPU state at the jump between terminal stages:

| Item | State |
|---|---|
| Mode | SVC |
| IRQ, FIQ | Masked in CPSR. Every source and FIQ routing disabled at the controller. |
| MMU | Off, even if the previous stage had it on |
| D-cache | Cleaned and invalidated, then off |
| I-cache | Invalidated. Off when the previous stage used `mmu.h`. |
| Timers | System timer channels 1 and 3 parked a full counter wrap ahead, their match flags cleared |
| UART | PL011 left on, FIFO drained |
| Framebuffer | Left on. Its address is in the boot info. |

`robco_handoff(entry, bi, fdt_or_atags)` in `src/arch/handoff.c` does steps 4 and 5, in this order:

1. Masks IRQ and FIQ on the CPU.
2. Disables FIQ routing and every IRQ source at the interrupt controller: pending 1, pending 2 and basic.
3. Sets system timer compare channels 1 and 3, the channels the ARM side owns, to one tick before the current counter value, so they cannot match for about 71 minutes. Then it clears their match flags. The next stage takes no stale timer interrupt when it unmasks a timer source.
4. If the MMU or D-cache is on and the image links `mmu_disable()`, which any image that uses `mmu.h` or `cache.h` does, it calls it. `mmu_disable()` cleans and invalidates the whole D-cache on every supported CPU, then turns the MMU, D-cache and I-cache off. Otherwise it turns the MMU and D-cache off, and cleans the D-cache first only on ARM1176.
5. Invalidates the I-cache and the branch predictor.
6. Jumps to `entry` in SVC mode with IRQ and FIQ masked and the registers below.

The caller may call `robco_handoff()` with the MMU and caches on or off when it turned them on with `mmu.h`. The next stage then starts with them off and every dirty line in memory. An image that turned the caches on by its own code, without linking `mmu.h` or `cache.h`, must clean them first on ARMv7 and later. The caller drains the UART with `uart_flush()`. The cache behaviour of the handoff is pending hardware validation.

Pip-OS enters ROMs and holotapes differently, in USR mode. See [PIPOS_ABI.md](./PIPOS_ABI.md).

### BOOT.CFG

`/ROBCO/BOOT.CFG` is INI text. This format is a draft.

```
[boot]
default = UOS
timeout = 3

[entry UOS]
path = /ROBCO/UOS.SYS
cmdline =

[entry MAINT]
path = /ROBCO/MAINT.SYS

[entry DIAG]
path = /ROBCO/DIAG.SYS
```

`timeout` is in seconds. If the file is missing or does not parse, MFBootAgent shows the menu with one entry, `/ROBCO/UOS.SYS`, and no timeout.

## Registers at entry

| Register | Value |
|---|---|
| r0 | `0` |
| r1 | Machine type the firmware passed in r1 |
| r2 | Address of the DTB or ATAGS copy, `0x00101000`, or `0` if there is none |
| r3 | `0x00100000`, the boot info address |

Firmware entry, for comparison: r0 is `0`, r1 is the machine type, r2 points at ATAGS at `0x100`, r3 is undefined.

## Entry API

Every image provides one C entry point:

```c
void robco_main(void);
```

`start.S` calls `robco_start()`. It detects the board, then calls `robco_main()`. If `robco_main()` returns, the image halts. See the startup sequence in [BOARD_SUPPORT.md](./BOARD_SUPPORT.md).

`robco_main()` takes no arguments. Code reads the entry state through accessors:

| Accessor | Header | Returns |
|---|---|---|
| `robco_boot_args()` | `robco/boot.h` | `robco_boot_args_t`: r0 to r3 at entry and the entry mode, CPSR bits [4:0] |
| `board_get()` | `robco/board.h` | `robco_board_t`: MIDR, SoC, peripheral base, VC bus alias, board revision, ARM memory |
| `bootinfo_get()` | `robco/bootinfo.h` | `const robco_bootinfo_t *`, the trusted block, the block the image built with `bootinfo_build()`, or `NULL` |
| `bootinfo_inherited()` | `robco/bootinfo.h` | 1 if the block from the previous stage passed the trust rule |

The accessors return pointers to data the library owns. Callers must not write through them.

## Boot info

The boot info block lives at `0x00100000` in the terminal chain. Pip-OS keeps its own copy inside its own range. The struct is version 2. It replaces `termlink_info_t` from MFBootAgent.

All fields are little-endian. The struct is packed with no padding. Strings are ASCII and NUL-terminated within their arrays.

```c
#include <stdint.h>

#define ROBCO_BOOTINFO_ADDR     0x00100000u
#define ROBCO_BOOTINFO_MAGIC    0x524F4243u  /* "CBOR" in memory, reads "ROBC" as a word */
#define ROBCO_BOOTINFO_VERSION  2u
#define ROBCO_HANDOFF_FDT_ADDR  0x00101000u
#define ROBCO_HANDOFF_FDT_MAX   0x0001F000u

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

_Static_assert(sizeof(robco_bootinfo_t) == 320, "robco_bootinfo_t must be 320 bytes");
```

| Offset | Size | Field | Meaning |
|---|---|---|---|
| `0x000` | 4 | `magic` | `0x524F4243` |
| `0x004` | 2 | `version` | `2` |
| `0x006` | 2 | `size` | `320`, the struct size |
| `0x008` | 4 | `crc32` | CRC-32/ISO-HDLC over `size` bytes with this field as 0 |
| `0x00C` | 4 | `stage` | Stage that wrote the block last. See stage ids. |
| `0x010` | 4 | `board_rev` | Raw value of mailbox tag `0x00010002` |
| `0x014` | 4 | `soc` | `0x2835`, `0x2836`, `0x2837` or `0x2711` |
| `0x018` | 4 | `periph_base` | Peripheral base from the detection table |
| `0x01C` | 4 | `midr` | Raw MIDR |
| `0x020` | 4 | `mem_base` | ARM memory base, mailbox tag `0x00010005` |
| `0x024` | 4 | `mem_size` | ARM memory size in bytes, same tag |
| `0x028` | 4 | `fw_r0` | r0 the firmware passed to the first stage |
| `0x02C` | 4 | `fw_r1` | r1 the firmware passed, the machine type |
| `0x030` | 4 | `fw_r2` | r2 the firmware passed, the original ATAGS or DTB address |
| `0x034` | 4 | `fdt_addr` | Address of the copy, `0x00101000`, or 0 |
| `0x038` | 4 | `fdt_size` | Size of the copy in bytes, or 0 |
| `0x03C` | 4 | `caps` | Capability bits. See below. |
| `0x040` | 4 | `uart_type` | Console UART: 0 none, 1 PL011, 2 mini UART |
| `0x044` | 4 | `uart_clock` | UART reference clock in Hz |
| `0x048` | 4 | `uart_baud` | Console baud rate, `115200` |
| `0x04C` | 4 | `fb_addr` | Framebuffer ARM physical address, bus alias cleared. 0 if none. |
| `0x050` | 4 | `fb_size` | Framebuffer size in bytes |
| `0x054` | 4 | `fb_width` | Width in pixels |
| `0x058` | 4 | `fb_height` | Height in pixels |
| `0x05C` | 4 | `fb_pitch` | Bytes per row |
| `0x060` | 4 | `fb_depth` | Bits per pixel |
| `0x064` | 4 | `fb_order` | Pixel order from the mailbox: 0 BGR, 1 RGB |
| `0x068` | 2 | `con_cols` | Text console columns |
| `0x06A` | 2 | `con_rows` | Text console rows |
| `0x06C` | 2 | `con_cx` | Cursor column, so the next stage continues the screen |
| `0x06E` | 2 | `con_cy` | Cursor row |
| `0x070` | 4 | `con_scheme` | Colour scheme: 0 green, 1 amber, 2 blue, 3 white |
| `0x074` | 4 | `boot_dev` | 0 unknown, 1 SD on EMMC, 2 SD on SDHOST |
| `0x078` | 4 | `boot_part_lba` | First LBA of the FAT boot partition |
| `0x07C` | 4 | `boot_flags` | Boot flag bits. See below. |
| `0x080` | 64 | `boot_path` | Path of the image loaded last, for example `/ROBCO/UOS.SYS` |
| `0x0C0` | 128 | `cmdline` | Command line from `BOOT.CFG`, may be empty |
| | 320 | | Total, `0x140` |

### Stage ids

| Value | Stage | `@RC` name |
|---|---|---|
| 0 | None | |
| 1 | RETROS-BIOS | `bios` |
| 2 | MFBootAgent | `bootagent` |
| 3 | UOS | `uos` |
| 4 | Maintenance image | `maint` |
| 5 | Diagnostics image | `diag` |
| 6 | Pip-OS | `pipos` |
| 7 | Deitrix | `deitrix` |

### caps bits

| Bit | Meaning |
|---|---|
| 0 | Framebuffer valid |
| 1 | PL011 initialised |
| 2 | Mini UART initialised |
| 3 | SD card initialised |
| 4 | Local interrupt controller at `0x40000000` present |
| 5 to 31 | Reserved, 0 |

### boot_flags bits

| Bit | Meaning |
|---|---|
| 0 | The loader used a fallback file, such as `MFBOOT.BAK` |
| 1 | The user asked for maintenance mode |
| 2 | The writing stage cold-booted and built the block itself |
| 3 to 31 | Reserved, 0 |

## Trust rule

A stage trusts the boot info only if all of these hold:

1. r3 at entry equals `0x00100000`.
2. `magic` is `0x524F4243`, `version` is 2 and `size` is 320.
3. The CRC matches.

`robco_start()` applies the trust rule before `robco_main()`, unless the project's `robco_config.h` sets `ROBCO_FIRST_STAGE` to 1. `bootinfo_inherited()` gives the result and `bootinfo_get()` the trusted block. A cold-booted image gets `NULL` from `bootinfo_get()` until it calls `bootinfo_build()`.

If any check fails, the stage cold-boots. It detects the board, queries the mailbox and builds a fresh block. It sets `boot_flags` bit 2. Then it continues as normal. This is why every stage can run alone.

RETROS-BIOS and Pip-OS are first stages. They always cold-boot and ignore r3. RAM can keep an old block across a watchdog reset, and the firmware does not define r3.

A trusting stage still reads MIDR. If MIDR differs from `midr`, it cold-boots.

Before the jump, the loader sets `stage` to its own id, updates the fields it changed, and recomputes the CRC. `bootinfo_build(bi, stage)` does this and seals the block. `bootinfo_seal()` recomputes the CRC after a later change.

A stage passes on the state it did not set up itself. Hardware such as a framebuffer stays active across the handoff, so its fields stay valid. The rule: a field that describes state this stage set up comes from this stage. Every other field is carried over from the trusted block. With no trusted block, the stage fills every field itself.

| Field group | Comes from this stage when |
|---|---|
| `stage`, `board_rev`, `soc`, `periph_base`, `midr`, `mem_base`, `mem_size`, caps bit 4 | Always |
| `fw_r0`, `fw_r1`, `fw_r2` | Cold boot only |
| `fdt_addr`, `fdt_size` | The stage copied the firmware blob |
| `uart_type`, `uart_clock`, `uart_baud`, caps bit 1 | The stage configured the PL011 |
| caps bit 2 | The stage initialised the mini UART |
| caps bit 3 | The stage initialised the SD card |
| `fb_*`, caps bit 0 | The stage called `fb_init()` |
| `con_cols`, `con_rows`, `con_cx`, `con_cy`, `con_scheme` | The stage's console is active |
| `boot_dev`, `boot_part_lba`, `boot_flags`, `boot_path`, `cmdline` | Cold boot only |
| Other caps bits | Never. They are reserved and 0. |

A stage that sets up a new framebuffer without a console clears `con_cols`, `con_rows`, `con_cx` and `con_cy`, since the old console drew on the old framebuffer. It keeps `con_scheme`. A loader that picks a boot device writes the `boot_*` fields after `bootinfo_build()` and calls `bootinfo_seal()`. `bootinfo_merge()` in `src/lib/bootinfo.c` holds the rule.

## DTB and ATAGS

The first stage copies what the firmware passed in r2 to `0x00101000`.

- If the word at r2 is `0xD00DFEED` in big-endian order, it is a DTB. The size is the big-endian `totalsize` at offset 4.
- Otherwise, if r2 points at an `ATAG_CORE` tag, `0x54410001`, it is an ATAGS list. The size runs to and includes the `ATAG_NONE` tag.
- Otherwise there is nothing to copy.

If the size is above `0x1F000`, the stage does not copy. It sets `fdt_addr` and `fdt_size` to 0 and passes r2 = 0. With no `.dtb` on the card, the firmware passes ATAGS.

Later stages pass `fdt_addr` on unchanged. No stage edits the copy.

`bootinfo_copy_fw_blob(dest, max)` applies these rules to the saved r2 and returns the size. `bootinfo_build()` records the copy in a cold-built block.

## Serial markers

Stages print machine-readable lines on the PL011 for tests. Each marker is one line.

```
marker = "@RC" SP "stage=" name SP "ev=" event *( SP key "=" value ) CRLF
name   = 1*( lower / DIGIT / "_" )
event  = 1*( lower / DIGIT / "_" )
key    = 1*( lower / DIGIT / "_" )
value  = 1*( VCHAR except SP )
```

- Numbers are hex with `0x`, or decimal for sizes, counts and clocks. Register values and addresses are always hex with 8 digits. `mem` in `boot` is hex too.
- `soc` is the lowercase SoC name: `bcm2835`, `bcm2836`, `bcm2837`. It is not the numeric `soc` field of the boot info.
- Paths are absolute and contain no spaces.
- Markers are always on. They are not behind a debug flag.
- A marker line never shares a line with other output. Code may print `\n` before a marker to end a partial line.
- The first marker of every image is `boot`.

Example:

```
@RC stage=bios ev=load path=/ROBCO/MFBOOT.SYS size=81920
@RC stage=bios ev=jump addr=0x00200000 next=bootagent
```

### Events

| Event | Keys | Meaning |
|---|---|---|
| `boot` | `midr soc base board_rev mem entry_mode` | First marker of every image. `entry_mode` is the CPU mode at entry, such as `svc` or `hyp`. |
| `handoff` | `trusted=0\|1` | Result of the trust rule. Not printed by first stages. |
| `load` | `path size` | Loader opened an image |
| `fallback` | `path` | Loader moves to a fallback file |
| `error` | `code path` | A load or validation failed. `code` uses the names from [IMAGE_FORMAT.md](./IMAGE_FORMAT.md). On an unsupported board `robco_start()` prints `code=unsupported_board midr` instead and halts. |
| `jump` | `addr next` | Last line before entering the next stage |
| `ready` | | Stage reached its main screen or prompt |
| `shell` | `reason` | Stage entered its emergency shell |
| `exception` | `type pc lr spsr dfsr dfar` | CPU exception. `type` is `undef`, `svc`, `prefetch_abort`, `data_abort`, `irq`, `fiq` or `reset`. The stage halts after it. |
| `panic` | `msg` | Fatal software error. `msg` uses `_` for spaces. |
| `holo_launch` | `path` | Pip-OS starts a holotape |
| `holo_exit` | `status` | Holotape returned |
| `holo_kill` | `reason` | Pip-OS killed a holotape |

| Stage | Must emit |
|---|---|
| `bios` | `boot`, `load`, `fallback`, `error`, `jump`, `shell` |
| `bootagent` | `boot`, `handoff`, `ready` when the menu shows, `load`, `error`, `jump` |
| `uos`, `maint`, `diag` | `boot`, `handoff`, `ready` |
| `pipos` | `boot`, `load`, `error`, `jump` for the ROM, `shell`, `holo_launch`, `holo_exit`, `holo_kill` |
| `deitrix` | `boot`, `ready` |

All stages emit `exception` and `panic` when those happen. Deitrix runs in USR mode, so it prints through the `log` syscall.

## Failure behaviour

| Stage | Failure | Action |
|---|---|---|
| RETROS-BIOS | `MFBOOT.SYS` fails any step | `ev=error`, `ev=fallback`, then try `MFBOOT.BAK` |
| RETROS-BIOS | `MFBOOT.BAK` also fails | `ev=error`, then the emergency shell |
| RETROS-BIOS | SD card or FAT mount fails | `ev=error code=io`, then the emergency shell |
| MFBootAgent | The chosen entry fails | `ev=error`. Shows the error in the menu and stays in the menu. |
| MFBootAgent | `BOOT.CFG` missing or bad | Menu with the default entry only |
| UOS | Boot info not trusted | Cold-boots and runs |
| Pip-OS | `ROM1.ROM` fails any step | `ev=error`, then the emergency shell |
| Pip-OS | A holotape fails any step | `ev=error`. The `launch` syscall returns an error to Deitrix. |
| Pip-OS | A holotape crashes | `ev=holo_kill`. Control returns to Deitrix in sandbox v2. In v1 the system halts with `ev=exception`. |
| Any | CPU exception in a stage | `ev=exception`, then halt |

The emergency shell runs on the PL011 and on the screen console. It accepts at least `help`, `info` and `reboot`. `info` prints the boot info fields. `reboot` uses the watchdog.
