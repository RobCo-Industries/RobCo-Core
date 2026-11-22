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
- Drivers: PL011 UART, system timer, interrupt controller, periodic tick, GPIO, reboot and halt, hardware RNG, framebuffer.
- A screen console with four colour schemes and two built-in 8x16 fonts, Fixedsys Excelsior by default and Spleen. See [FONTS.md](./docs/FONTS.md).
- Hardware-independent code: klibc, `k_printf`, CRC-32, a heap, `rimg` image validation and the boot info block.
- `tools/rimg.py` to seal and check images. Every build seals its image.
- Examples in `examples/`, a QEMU test harness, host unit tests, target tests on four QEMU boards, a canary suite and a pinned CI container.

The [specification index](./docs/spec/README.md) lists what is specified but not implemented yet. That includes SD card storage, stage loading from the card, input, audio, the MMU and the Pip-OS ABI.

## Run the tests

You need `arm-none-eabi-gcc`, `qemu-system-arm` and `qemu-system-aarch64` 10.1.x, mtools and Python 3.

```sh
make venv && make test
```

## Licence

MIT. See [LICENSE](./LICENSE).
