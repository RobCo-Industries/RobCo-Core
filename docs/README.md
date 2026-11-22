# Documentation

## Guide

| File | Content |
|---|---|
| [guide/GETTING_STARTED.md](./guide/GETTING_STARTED.md) | Tools, a first project on the library, QEMU, a first test, an SD card |
| [guide/API.md](./guide/API.md) | Every public function, type and constant, with init order and context rules |
| [guide/PROJECT_LAYOUT.md](./guide/PROJECT_LAYOUT.md) | Repository map, where new code and tests go, what CI runs |

## Specifications

| File | Content |
|---|---|
| [spec/README.md](./spec/README.md) | Index of the contracts and which parts the code implements |
| [spec/BOARD_SUPPORT.md](./spec/BOARD_SUPPORT.md) | Boards, runtime detection, UART, `config.txt`, startup sequence |
| [spec/MEMORY_MAP.md](./spec/MEMORY_MAP.md) | Address ranges of both boot chains |
| [spec/IMAGE_FORMAT.md](./spec/IMAGE_FORMAT.md) | The `rimg` format, validation, SD card files, `tools/rimg.py` |
| [spec/BOOT_PROTOCOL.md](./spec/BOOT_PROTOCOL.md) | Stage loading, register handoff, boot info, `@RC` markers |
| [spec/PIPOS_ABI.md](./spec/PIPOS_ABI.md) | Pip-OS entry state and syscalls. Draft, not implemented. |
| [spec/TESTING.md](./spec/TESTING.md) | Test layers, QEMU boot method, canary suite, CI |

## Other

| File | Content |
|---|---|
| [FONTS.md](./FONTS.md) | Console fonts and how to build a font table |
| [../tests/README.md](../tests/README.md) | Running and writing host, target and canary tests |
| [../ci/README.md](../ci/README.md) | The CI container and its tag |
| [../spikes/s1-uart-hello/README.md](../spikes/s1-uart-hello/README.md) | Spike 1: UART hello on every board |
| [../spikes/s2-sd-read/README.md](../spikes/s2-sd-read/README.md) | Spike 2: SD sector 0 through the EMMC controller |
| [../spikes/s3-fb-mmu/README.md](../spikes/s3-fb-mmu/README.md) | Spike 3: framebuffer and MMU fill rate |
