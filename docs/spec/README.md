# Specifications

These files are the contracts for RobCo-Core and every repo built on it. Code follows them. A change to a contract changes the file first.

| File | Covers | Status |
|---|---|---|
| [BOARD_SUPPORT.md](./BOARD_SUPPORT.md) | Boards, runtime detection, UART, `config.txt`, startup sequence | stable |
| [MEMORY_MAP.md](./MEMORY_MAP.md) | Address ranges for both chains, lore vs real numbers | stable |
| [IMAGE_FORMAT.md](./IMAGE_FORMAT.md) | The `rimg` format, validation order, SD card files, `tools/rimg.py` | stable |
| [BOOT_PROTOCOL.md](./BOOT_PROTOCOL.md) | Stage loading, register handoff, entry API, boot info, `@RC` markers, failure handling | stable |
| [PIPOS_ABI.md](./PIPOS_ABI.md) | Pip-OS entry state, syscalls, display, saves, sandbox, SDK | draft |
| [TESTING.md](./TESTING.md) | Test layers, QEMU boot methods, canary suite, SD images, CI, hardware checklists | stable |

Spots marked "pending hardware validation" may change after tests on real boards.

## What the code implements

Each table lists the parts of one document. "Implemented" means the code in this repo does it today. "Specified" means the document defines it and no code does it yet.

### BOARD_SUPPORT.md

| Part | State |
|---|---|
| Startup sequence in `src/arch/start.S` | Implemented |
| MIDR detection, `board_get()` | Implemented |
| Mailbox board revision, ARM memory and UART clock | Implemented |
| PL011 console, pin mux and divisor | Implemented |
| `mmio_probe()` | Implemented |
| System timer, `timer_*()` | Implemented |
| Interrupt controller, `irq_*()` | Implemented |
| Periodic tick on timer channel 1, `tick_*()` | Implemented |
| GPIO functions, levels and pulls | Implemented |
| Reboot and halt through the PM watchdog | Implemented |
| Hardware RNG | Implemented |
| Framebuffer through the mailbox, `fb_*()` | Implemented |
| 8x16 font and screen console, `console_*()` | Implemented |
| `unsupported board` report on BCM2711: `ev=error code=unsupported_board`, then halt, in `src/board/board.c` | Implemented |
| Board name from the revision code | Specified |
| Mini UART maintenance port | Specified |
| SD card storage on EMMC or SDHOST | Specified |
| Input, PWM audio | Specified |

### MEMORY_MAP.md

| Part | State |
|---|---|
| Per-image `.stack` section and `mem_size` | Implemented |
| QEMU firmware stub range | Implemented |
| `__heap_start` and the heap allocator, `k_malloc()` | Implemented |
| Slot end checked at link time with `ROBCO_SLOT_END` | Implemented |
| Heap regions within the map, `robco_heap_slot()` and `robco_heap_high()` | Implemented |
| MMU with an identity map | Specified |
| Slot bounds checked by `rimg_validate()` and `tools/rimg.py` | Implemented |
| Handoff range: boot info at `0x00100000`, DTB or ATAGS copy at `0x00101000` | Implemented |

### IMAGE_FORMAT.md

| Part | State |
|---|---|
| Branch stub and `rimg_header_t` emitted by `start.S` | Implemented |
| Magic and subtype constants in `rimg.h` | Implemented |
| META block | Specified |
| CRC-32 function, `crc32()` | Implemented |
| Image CRC check, validation, `rimg_err_t`, `rimg_validate()` | Implemented |
| `tools/rimg.py` `seal`, `info` and `verify` | Implemented |
| Sealing every image in `mk/robco.mk` | Implemented |
| Filling the META block in `tools/rimg.py` | Specified |
| Loading images from the SD card | Specified |

### BOOT_PROTOCOL.md

| Part | State |
|---|---|
| `robco_main(void)`, `robco_boot_args()`, `board_get()` | Implemented |
| `ev=exception` marker | Implemented |
| Boot info block: `robco_bootinfo_t`, trust rule, `bootinfo_get()` | Implemented |
| Register handoff, `robco_handoff()` | Implemented |
| DTB and ATAGS copy, `bootinfo_copy_fw_blob()` | Implemented |
| Stage loading from the SD card, `BOOT.CFG` | Specified |
| Other `@RC` events, failure handling, emergency shell | Specified |

### PIPOS_ABI.md

| Part | State |
|---|---|
| Everything | Specified |

### TESTING.md

| Part | State |
|---|---|
| Canary suite | Implemented |
| Firmware stub and `boot()` | Implemented |
| `sdimage.py` | Implemented |
| Serial tests in `tests/target/` | Implemented |
| Host tests for klibc, CRC-32, the allocator, `rimg_validate()`, the boot info and the tick conversion | Implemented |
| Unit tests for `tools/rimg.py` | Implemented |
| Two-stage chain boot in QEMU | Implemented |
| Console grid dump and a screendump golden | Implemented |
| Fault tests: bad magic, bad CRC, out-of-slot images and a missing next stage in the chain, an undefined instruction in `hello` | Implemented |
| Other host tests, input tests, other fault tests | Specified |
| `expect.py`, `screen.py`, `inputs.py` | Specified |
| Reusable workflow for sibling repos | Implemented |
