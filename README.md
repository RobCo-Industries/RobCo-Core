# RobCo-Core

Shared bare-metal code for the RobCo Industries suite. The suite rebuilds the RobCo software from the Fallout games on Raspberry Pi, with no Linux underneath.

Two boot chains will use it:

- Terminal: [RETROS-BIOS](https://github.com/RobCo-Industries/RETROS-BIOS) → [MFBootAgent](https://github.com/RobCo-Industries/MFBootAgent) → [UOS](https://github.com/RobCo-Industries/UOS)
- Pip-Boy: [Pip-OS](https://github.com/RobCo-Industries/Pip-OS) → [Deitrix](https://github.com/RobCo-Industries/Deitrix)

## What exists today

- The first slice of the library: `include/robco/`, `src/`, the `mk/robco.mk` build fragment and the `ld/` linker script. It covers startup, board detection, the mailbox, the PL011 console, exception reports and the `rimg` header.
- An example image in `examples/hello/`.
- A QEMU test harness in `tests/harness/`, with a firmware stub in `tests/fwstub/`.
- A canary suite in `tests/canary/` that checks the QEMU behaviour the tests rely on.
- Target tests in `tests/target/` that boot the spikes and the example on every board.
- A pinned CI container in `ci/` and a reusable workflow for the sibling repos.
- Three spikes in `spikes/`: UART hello, SD sector read, framebuffer and MMU. They run in QEMU on `raspi0`, `raspi1ap`, `raspi2b` and `raspi3b`.
- The specifications in [docs/spec/](./docs/spec/README.md). Its index shows which parts the code implements.

## What does not exist yet

The boot info block, stage loading, `tools/rimg.py` and the Pip-OS ABI. The spikes are throwaway code that proves the drivers work.

## Run the tests

You need `qemu-system-arm` and `qemu-system-aarch64` 10.1.x, `arm-none-eabi-gcc` and Python 3.

```sh
make venv && make test
```

`make test` runs the canary suite, builds the example warning-free, and boots every spike and the example on every QEMU machine.

## Licence

MIT. See [LICENSE](./LICENSE).
