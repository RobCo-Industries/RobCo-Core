# Specifications

These files are the contracts for RobCo-Core and every repo built on it. Code follows them. A change to a contract changes the file first.

| File | Covers | Status |
|---|---|---|
| [BOARD_SUPPORT.md](./BOARD_SUPPORT.md) | Boards, runtime detection, UART, `config.txt`, startup sequence | stable |
| [MEMORY_MAP.md](./MEMORY_MAP.md) | Address ranges for both chains, lore vs real numbers | stable |
| [IMAGE_FORMAT.md](./IMAGE_FORMAT.md) | The `rimg` format, validation order, SD card files, `tools/rimg.py` | stable |
| [BOOT_PROTOCOL.md](./BOOT_PROTOCOL.md) | Stage loading, register handoff, boot info, `@RC` markers, failure handling | stable |
| [PIPOS_ABI.md](./PIPOS_ABI.md) | Pip-OS entry state, syscalls, display, saves, sandbox, SDK | draft |
| [TESTING.md](./TESTING.md) | Test layers, QEMU boot methods, canary suite, SD images, CI, hardware checklists | stable |

Spots marked "pending hardware validation" may change after tests on real boards.
