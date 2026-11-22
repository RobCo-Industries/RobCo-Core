# RobCo-Core

Shared bare-metal code for the RobCo Industries suite. The suite rebuilds the RobCo software from the Fallout games on Raspberry Pi, with no Linux underneath. One AArch32 image runs on every Pi from the Zero to the 3 and detects its board at runtime.

Two boot chains will use it:

- Terminal: [RETROS-BIOS](https://github.com/RobCo-Industries/RETROS-BIOS) → [MFBootAgent](https://github.com/RobCo-Industries/MFBootAgent) → [UOS](https://github.com/RobCo-Industries/UOS)
- Pip-Boy: [Pip-OS](https://github.com/RobCo-Industries/Pip-OS) → [Deitrix](https://github.com/RobCo-Industries/Deitrix)

## Start here

- [Getting started](./docs/guide/GETTING_STARTED.md): build a first project on the library and test it in QEMU.
- [API reference](./docs/guide/API.md): every public function.
- [Documentation index](./docs/README.md): the guide, the specifications and the test docs.

## What exists today

- Startup, board detection, the mailbox, exception reports and the stage handoff.
- Drivers: PL011 UART and mini UART, system timer, interrupt controller, periodic tick, GPIO, reboot and halt, hardware RNG, framebuffer.
- An identity-mapped MMU with data and instruction caches, and cache maintenance for buffers shared with the GPU.
- Storage: a block device interface, an SD card driver on the EMMC controller, a RAM disk, MBR partitions, a FAT12/16/32 file system over FatFs and a stage loader that loads the next image from the card with a fallback file.
- Input from GPIO buttons, a rotary dial and serial lines, as one event queue.
- Square-wave audio on PWM, with tones, note sequences and RTTTL tunes, and a trace backend for tests.
- An I2C master, a DS1307, DS1338 or DS3231 real-time clock with a software clock, an SPI master, date and time functions and an INI parser.
- A screen console with four colour schemes and two built-in 8x16 fonts, Fixedsys Excelsior by default and Spleen. See [FONTS.md](./docs/FONTS.md).
- Hardware-independent code: klibc, `k_printf`, CRC-32, a heap, `rimg` image validation and the boot info block.
- `tools/rimg.py` to seal, pack and check images, META title and icon included. Every build seals its image. `tools/mksdimage.sh` builds an SD card image, also through the optional `make sdimage` target of `mk/robco.mk`, which a project extends with its own files.
- Examples in `examples/`, a QEMU test harness, host unit tests, target tests on four QEMU boards, a canary suite and a pinned CI container.
- For projects that use the library as a submodule: host tests through `mk/host.mk`, a pytest plugin and an interactive runner in `tests/harness`, project commands on the script channel and `tools/check_pins.sh` for the CI pins. [Getting started](./docs/guide/GETTING_STARTED.md) shows each one.

The [specification index](./docs/spec/README.md) lists what is specified but not implemented yet. That includes the `BOOT.CFG` boot menu, the board name from the revision code and the Pip-OS ABI. What QEMU cannot prove, such as sound and cache behaviour, is listed in [HARDWARE_VALIDATION.md](./docs/HARDWARE_VALIDATION.md).

## Run the tests

You need `arm-none-eabi-gcc`, `qemu-system-arm` and `qemu-system-aarch64` 10.1.x, mtools and Python 3.

```sh
make venv && make test
```

| Layer | Command | Runs |
|---|---|---|
| Host | `make test-host` | C unit tests for `src/lib/` and the file system, with ASan and UBSan |
| Tools | `make test-tools` | pytest for `tools/`, the Makefile and the file system against mtools |
| Docs | `make test-docs` | Links, API coverage, checked commands and banned words |
| Canary | `make test-canary` | The QEMU behaviour the tests rely on |
| Spikes | `make test-spikes` | The three hardware spikes on every QEMU board |
| Examples | `make test-examples` | Every example and variant, built warning-free |
| Target | `make test-target` | Every example booted in QEMU on four boards. `BOARD=raspi0` picks one. |

[tests/README.md](./tests/README.md) explains each layer.

## Licence

MIT. See [LICENSE](./LICENSE).
