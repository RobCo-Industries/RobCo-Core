# RobCo-Core

[![nightly](https://github.com/RobCo-Industries/RobCo-Core/actions/workflows/nightly.yml/badge.svg)](https://github.com/RobCo-Industries/RobCo-Core/actions/workflows/nightly.yml)

Shared bare-metal code for the RobCo Industries suite. The suite rebuilds the RobCo software from the Fallout games on Raspberry Pi, with no Linux underneath. One AArch32 image runs on every Pi from the Zero to the 3 and detects its board at runtime.

Five sibling repositories use it, in two boot chains. Four mount it as the submodule `external/robco-core`. Deitrix gets it through Pip-OS.

- Terminal: [RETROS-BIOS](https://github.com/RobCo-Industries/RETROS-BIOS) → [MFBootAgent](https://github.com/RobCo-Industries/MFBootAgent) → [UOS](https://github.com/RobCo-Industries/UOS)
- Pip-Boy: [Pip-OS](https://github.com/RobCo-Industries/Pip-OS) → [Deitrix](https://github.com/RobCo-Industries/Deitrix)

A nightly run builds the five siblings and boots both chains in QEMU. [Nightly](./docs/spec/TESTING.md#nightly) says what it builds and runs.

## Start here

- [Getting started](./docs/guide/GETTING_STARTED.md): build a first project on the library and test it in QEMU.
- [API reference](./docs/guide/API.md): every public function.
- [Documentation index](./docs/README.md): the guide, the specifications and the test docs.

## What exists today

- Startup, board detection, the mailbox, exception reports and the stage handoff.
- Drivers: PL011 UART and mini UART, system timer, interrupt controller, periodic tick, GPIO, reboot and halt, a hardware watchdog, hardware RNG, framebuffer, a PCA9554 I2C GPIO expander and an ILI9341 SPI panel.
- An identity-mapped MMU with data and instruction caches, and cache maintenance for buffers shared with the GPU.
- Storage: a block device interface, an SD card driver on the EMMC controller, a read-only SD card driver over SPI0 for a removable holotape slot, a RAM disk, a second file system volume that can leave while the device runs, MBR partitions, a FAT12/16/32 file system over FatFs and a stage loader that loads the next image from the card with a fallback file.
- Input from GPIO buttons, a rotary dial and serial lines, as one event queue.
- Audio on PWM: tones, note sequences, RTTTL tunes, 8-bit PCM samples up to 37500 Hz with a queue and an overlay, notes computed live, an optional rest at the midpoint level, and a trace backend for tests. None of it was heard on a board yet.
- User-mode tasks with `umode_run()`, with or without the MMU.
- An I2C master, a DS1307, DS1338 or DS3231 real-time clock with a software clock, an SPI master, date and time functions and an INI parser.
- A screen console with four colour schemes and two built-in 8x16 fonts, Fixedsys Excelsior by default and Spleen. See [FONTS.md](./docs/FONTS.md).
- Hardware-independent code: klibc, `k_printf`, CRC-32, a heap, `rimg` image validation, the sealer and packer `rimg_seal()` and `rimg_pack()`, and the boot info block.
- `tools/rimg.py` to seal, pack and check images, META title and icon included. Every build seals its image. `tools/mksdimage.sh` builds an SD card image, also through the optional `make sdimage` target of `mk/robco.mk`, which a project extends with its own files.
- Examples in `examples/`, a QEMU test harness, host unit tests, target tests on four QEMU boards, a canary suite and a pinned CI container.
- Sound tools: `tools/sfx_table.py` renders a sound effect model into 8-bit tables, `tools/sfx_check.py` checks tables against their model, and `tools/audio_preview.py` turns a serial log's audio trace into a WAV file.
- The nightly run in `.github/workflows/nightly.yml`, with its scripts in `tools/nightly/`.
- For projects that use the library as a submodule: host tests through `mk/host.mk`, a pytest plugin and an interactive runner in `tests/harness`, `tools/check_pins.sh` to check the CI pins against the submodule, `tools/check_doc_links.sh` to check doc links to a pinned repository and `tools/check_workflow_exprs.py` for safe, pinned workflows. [Getting started](./docs/guide/GETTING_STARTED.md) shows the host tests, the harness and the two pin checks.
- [PROJECT_LAYOUT.md](./docs/guide/PROJECT_LAYOUT.md) lists the other tools under `tools/`.

The [specification index](./docs/spec/README.md) lists what is specified but not implemented yet. That includes the `BOOT.CFG` boot menu, the board name from the revision code and the Pip-OS ABI. ## Hardware

Three examples ran on a Pi Zero W 1.1: `hello`, `sdraw` and `umode`. No other board has run the library yet. [HARDWARE_VALIDATION.md](./docs/HARDWARE_VALIDATION.md) holds the results and lists what QEMU cannot prove, such as sound and cache behaviour.

## Run the tests

You need `arm-none-eabi-gcc`, `qemu-system-arm` and `qemu-system-aarch64` 10.1.x, mtools and Python 3.

```sh
make venv && make test
```

| Layer | Command | Runs |
|---|---|---|
| Host | `make test-host` | C unit tests for `src/lib/` and the file system, with ASan and UBSan |
| Tools | `make test-tools` | pytest for `tools/`, the Makefile and the file system against mtools |
| Workflows | `make check-workflows` | No unsafe `${{ }}` in the workflows, and every action pinned by commit |
| Docs | `make test-docs` | Links, API coverage, checked commands and banned words |
| Canary | `make test-canary` | The QEMU behaviour the tests rely on |
| Spikes | `make test-spikes` | The three hardware spikes on every QEMU board |
| Examples | `make test-examples` | Every example and variant, built warning-free |
| Target | `make test-target` | Every example booted in QEMU on four boards. `BOARD=raspi0` picks one. |
| CI | `make ci-local` | CI's host job in the `robco-ci` container with Docker. `make ci-local-image` builds the image. |

[tests/README.md](./tests/README.md) explains each layer.

## Licence

MIT. See [LICENSE](./LICENSE).
