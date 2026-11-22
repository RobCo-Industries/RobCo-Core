# RobCo-Core

Shared bare-metal code for the RobCo Industries suite. The suite rebuilds the RobCo software from the Fallout games on Raspberry Pi, with no Linux underneath.

Two boot chains will use it:

- Terminal: [RETROS-BIOS](https://github.com/RobCo-Industries/RETROS-BIOS) → [MFBootAgent](https://github.com/RobCo-Industries/MFBootAgent) → [UOS](https://github.com/RobCo-Industries/UOS)
- Pip-Boy: [Pip-OS](https://github.com/RobCo-Industries/Pip-OS) → [Deitrix](https://github.com/RobCo-Industries/Deitrix)

## What exists today

- A QEMU test harness in `tests/harness/`.
- A canary suite in `tests/canary/` that checks the QEMU behaviour the tests rely on.
- A pinned CI container in `ci/` and a reusable workflow for the sibling repos.
- Three spikes in `spikes/`: UART hello, SD sector read, framebuffer and MMU. They run in QEMU on `raspi0`, `raspi1ap`, `raspi2b` and `raspi3b`.
- The specifications in [docs/spec/](./docs/spec/README.md).

## What does not exist yet

The library itself. There is no `include/robco/`, no `librobco.a` and no `tools/rimg.py` yet. The spikes are throwaway code that proves the drivers work.

## Run the tests

You need `qemu-system-arm` and `qemu-system-aarch64` 10.1.x, `arm-none-eabi-gcc` and Python 3.

```sh
make venv && make test
```

`make test` runs the canary suite and boots every spike on every QEMU machine.

## Licence

MIT. See [LICENSE](./LICENSE).
