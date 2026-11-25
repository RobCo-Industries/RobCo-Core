# Specifications

These files are the contracts for RobCo-Core and every repo built on it. Code follows them. A change to a contract changes the file first.

| File | Covers | Status |
|---|---|---|
| [BOARD_SUPPORT.md](./BOARD_SUPPORT.md) | Boards, runtime detection, UART, `config.txt`, startup sequence | stable |
| [MEMORY_MAP.md](./MEMORY_MAP.md) | Address ranges for both chains, lore vs real numbers | stable |
| [IMAGE_FORMAT.md](./IMAGE_FORMAT.md) | The `rimg` format, validation order, SD card files, `tools/rimg.py`, sealing and packing rules with test vectors | stable |
| [BOOT_PROTOCOL.md](./BOOT_PROTOCOL.md) | Stage loading, register handoff, entry API, boot info, `@RC` markers, failure handling | stable |
| [PIPOS_ABI.md](./PIPOS_ABI.md) | Pip-OS entry state, syscalls, display, saves, sandbox, SDK | draft |
| [TESTING.md](./TESTING.md) | Test layers, QEMU boot methods, `boot.json`, canary suite, SD images, CI, hardware checklists | stable |
| [STUDIO_PROJECT.md](./STUDIO_PROJECT.md) | The Studio project files `robco.toml` and `robco.lock` | draft |
| [SDK_PACKAGE.md](./SDK_PACKAGE.md) | SDK packages, `sdk.json`, `flags.json`, the firmware pins, the SDK index, version and hash rules | draft |

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
| Mini UART driver, `uart_mini_*()`, used by the input script backend in QEMU | Implemented |
| Mini UART maintenance port on hardware | Specified |
| SD card on EMMC, `sd_*()`, with the pin routing for WiFi boards | Implemented, pending hardware validation |
| SD card on SDHOST | Specified |
| Default pins for I2C, SPI and audio | Implemented |
| Input from GPIO buttons, a dial and serial lines, `input_*()` | Implemented, pending hardware validation |
| PWM audio with a trace backend, `audio_*()` | Implemented, pending hardware validation |
| I2C, SPI, DS1307, DS1338 and DS3231 RTC | Implemented, pending hardware validation |

### MEMORY_MAP.md

| Part | State |
|---|---|
| Per-image `.stack` section and `mem_size` | Implemented |
| QEMU firmware stub range | Implemented |
| `__heap_start` and the heap allocator, `k_malloc()` | Implemented |
| Slot end checked at link time with `ROBCO_SLOT_END` | Implemented |
| Heap regions within the map, `robco_heap_slot()` and `robco_heap_high()` | Implemented |
| MMU with an identity map and memory types per region, `mmu_*()` | Implemented, pending hardware validation |
| Cache maintenance for buffers shared with the GPU, `cache_*()` | Implemented, pending hardware validation |
| Slot bounds checked by `rimg_validate()` and `tools/rimg.py` | Implemented |
| Handoff range: boot info at `0x00100000`, DTB or ATAGS copy at `0x00101000` | Implemented |

### IMAGE_FORMAT.md

| Part | State |
|---|---|
| Branch stub and `rimg_header_t` emitted by `start.S` | Implemented |
| Magic and subtype constants in `rimg.h` | Implemented |
| META block | Implemented |
| CRC-32 function, `crc32()` | Implemented |
| Image CRC check, validation, `rimg_err_t`, `rimg_validate()` | Implemented |
| `tools/rimg.py` `seal`, `info` and `verify` | Implemented |
| Sealing every image in `mk/robco.mk` | Implemented |
| Firmware download with pinned hashes, `tools/fetch_firmware.sh`, and boot directory, `tools/mkbootdir.sh` | Implemented |
| Filling the META block in `tools/rimg.py` | Implemented |
| Loading images from the SD card, `loader_load()` | Implemented, pending hardware validation |
| Sealing and packing rules in `tools/rimg.py` | Implemented |
| Seal and pack test vectors in `tests/vectors/rimg/`, checked against `tools/rimg.py` | Implemented |
| `rimg_seal()` and `rimg_pack()` in C | Specified |

### BOOT_PROTOCOL.md

| Part | State |
|---|---|
| `robco_main(void)`, `robco_boot_args()`, `board_get()` | Implemented |
| `ev=exception` marker | Implemented |
| Boot info block: `robco_bootinfo_t`, trust rule, `bootinfo_get()` | Implemented |
| Register handoff, `robco_handoff()`, including turning the MMU and caches off | Implemented |
| DTB and ATAGS copy, `bootinfo_copy_fw_blob()` | Implemented |
| Stage loading from the SD card with fallback, `loader_load_fallback()`, in `examples/chain1` | Implemented, pending hardware validation |
| `BOOT.CFG` | Specified |
| `mount`, `load`, `fallback`, `error`, `shell`, `halt` and `jump` markers and the RETROS-BIOS failure rows, in `examples/chain1` | Implemented |
| Emergency shell, a job of each stage | Specified |
| Other `@RC` events | Specified |

### PIPOS_ABI.md

| Part | State |
|---|---|
| Everything | Specified |
| Hiding calls newer than `PIPOS_ABI_TARGET` in the SDK headers | Specified |

### TESTING.md

| Part | State |
|---|---|
| Canary suite | Implemented |
| Firmware stub and `boot()` | Implemented |
| `sdimage.py` | Implemented |
| Serial tests in `tests/target/` | Implemented |
| Host tests for klibc, CRC-32, the allocator, `rimg_validate()`, the stage loader, the boot info, the tick conversion, partitions, the file system, input, audio, date and time and INI | Implemented |
| Tool tests, including the file system against mtools | Implemented |
| Input tests over the console and script backends | Implemented |
| Target tests with an SD image, the RTC model and the SPI flash | Implemented |
| Unit tests for `tools/rimg.py` | Implemented |
| Two-stage chain boot in QEMU, also from an SD image with fallback | Implemented |
| Console grid dump and a screendump golden | Implemented |
| Fault tests: bad magic, bad CRC, out-of-slot images and a missing next stage in the chain, an undefined instruction in `hello`, MMU faults | Implemented |
| Game logic and UI host tests, file fault tests | Specified |
| `expect.py`, `screen.py`, `inputs.py` | Specified |
| Reusable workflow for sibling repos | Implemented |
| `boot.json` and its writer in the harness | Specified |

### STUDIO_PROJECT.md

| Part | State |
|---|---|
| Everything. No tool reads or writes these files yet. | Specified |

### SDK_PACKAGE.md

| Part | State |
|---|---|
| Everything. No package or index is built yet. | Specified |
