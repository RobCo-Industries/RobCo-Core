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
| `unsupported board` panic on BCM2711 | Specified |
| Board name from the revision code | Specified |
| Mini UART maintenance port | Specified |

### MEMORY_MAP.md

| Part | State |
|---|---|
| Per-image `.stack` section and `mem_size` | Implemented |
| QEMU firmware stub range | Implemented |
| Terminal and Pip-Boy chain slots | Specified |

### IMAGE_FORMAT.md

| Part | State |
|---|---|
| Branch stub and `rimg_header_t` emitted by `start.S` | Implemented |
| Magic and subtype constants in `rimg.h` | Implemented |
| META block | Specified |
| CRC, validation, `rimg_err_t` | Specified |
| `tools/rimg.py` | Specified |

### BOOT_PROTOCOL.md

| Part | State |
|---|---|
| `robco_main(void)`, `robco_boot_args()`, `board_get()` | Implemented |
| `ev=exception` marker | Implemented |
| `robco_bootinfo_t`, trust rule, `robco_bootinfo()` | Specified |
| Stage loading, DTB and ATAGS copy, `BOOT.CFG` | Specified |
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
| Host tests, screen goldens, input tests, fault tests | Specified |
| `expect.py`, `screen.py`, `inputs.py` | Specified |
| Reusable workflow for sibling repos | Implemented |
