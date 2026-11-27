# Documentation

## Guide

| File | Content |
|---|---|
| [guide/GETTING_STARTED.md](./guide/GETTING_STARTED.md) | Tools, a first project on the library, QEMU, a first test, an SD card |
| [guide/API.md](./guide/API.md) | Every public function, type and constant, with init order and context rules. Topic files in [guide/api/](./guide/api/). |
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
| [spec/SFX_MODEL.md](./spec/SFX_MODEL.md) | Sound effect models and `tools/sfx_table.py` |
| [spec/TESTING.md](./spec/TESTING.md) | Test layers, QEMU boot method, canary suite, CI, the nightly run |
| [spec/STUDIO_PROJECT.md](./spec/STUDIO_PROJECT.md) | The Studio project files `robco.toml` and `robco.lock`. Draft, not implemented. |
| [spec/SDK_PACKAGE.md](./spec/SDK_PACKAGE.md) | SDK packages, the cache, the runner ROM and the SDK index. Draft, not implemented. |
| [spec/STUDIO_CLI.md](./spec/STUDIO_CLI.md) | The `robco` command line. Draft, not implemented. |

## Other

| File | Content |
|---|---|
| [FONTS.md](./FONTS.md) | Console fonts and how to build a font table |
| [HARDWARE_VALIDATION.md](./HARDWARE_VALIDATION.md) | What QEMU cannot prove, per component, and how to check it on a real board |
| [../tests/README.md](../tests/README.md) | Running and writing host, target and canary tests |
| [../ci/README.md](../ci/README.md) | The CI container and its tag |
| [../spikes/s1-uart-hello/README.md](../spikes/s1-uart-hello/README.md) | Spike 1: UART hello on every board |
| [../spikes/s2-sd-read/README.md](../spikes/s2-sd-read/README.md) | Spike 2: SD sector 0 through the EMMC controller |
| [../spikes/s3-fb-mmu/README.md](../spikes/s3-fb-mmu/README.md) | Spike 3: framebuffer and MMU fill rate |
